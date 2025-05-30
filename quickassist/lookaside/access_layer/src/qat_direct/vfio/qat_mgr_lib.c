/*****************************************************************************
 *
 *   BSD LICENSE
 * 
 *   Copyright(c) 2007-2022 Intel Corporation. All rights reserved.
 *   All rights reserved.
 * 
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 * 
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 *
 *****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <sys/sysinfo.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <search.h>
#include <grp.h>
#include <numa.h>

#include "adf_pfvf_vf_msg.h"
#include "icp_accel_devices.h"
#include "icp_platform.h"
#include "qat_log.h"
#include "qat_mgr.h"
#include "vfio_lib.h"
#include "adf_vfio_pf.h"


#define INTEL_VENDOR_ID 0x8086
#define QAT_4XXXVF_DEVICE_ID 0x4941
#define QAT_401XXVF_DEVICE_ID 0x4943
#define QAT_402XXVF_DEVICE_ID 0x4945
#define QAT_420XXVF_DEVICE_ID 0x4947


#define IOMMUGROUP_DEV_DIR "/sys/kernel/iommu_groups/%s/devices/"
#define DEVVFIO_DIR "/dev/vfio"

#define DEVICE_FILE IOMMUGROUP_DEV_DIR "%s/device"
#define VENDOR_FILE IOMMUGROUP_DEV_DIR "%s/vendor"
#define NUMA_NODE IOMMUGROUP_DEV_DIR "%s/numa_node"

#define SYSFS_VF_DIR "/sys/bus/pci/devices"

#define SYSFS_VF_UEVENT "physfn/uevent"
#define PCI_DEV_SLOT_NAME "PCI_SLOT_NAME"

/* clang-format off */
#define PF_DEVICE_FORMAT PCI_DEV_SLOT_NAME"=%s"
/* clang-format on */

#define VFIO_ENTRY "vfio"
#define PF_INFO_UNINITIALISED (-1)

static struct qatmgr_section_data *section_data = NULL;
static int num_section_data = 0;
STATIC icp_accel_pf_info_t pf_data[ADF_MAX_PF_DEVICES] = { 0 };
STATIC int32_t num_pfs = PF_INFO_UNINITIALISED;
/* node_cpu_data contains available cpu ids for each node */
static struct qatmgr_cpu_data *cpu_data = NULL;
static int num_nodes = 0;
static int num_cpus = 0;


static pthread_mutex_t section_data_mutex;
/* message name within array should lives at index determined by it's
 * message define from qat_mgr.h */
static const char *qatmgr_msgtype_str[] = {
    "QATMGR_MSGTYPE_UNKNOWN",       /* string for unknown msg*/
    "QATMGR_MSGTYPE_SECTION_GET",   /* string for get section msg*/
    "QATMGR_MSGTYPE_SECTION_PUT",   /* string for put section msg*/
    "QATMGR_MSGTYPE_NUM_DEVICES",   /* string for num devices msg*/
    "QATMGR_MSGTYPE_DEVICE_INFO",   /* string for device info msg*/
    "QATMGR_MSGTYPE_DEVICE_ID",     /* string for device id msg*/
    "QATMGR_MSGTYPE_RESERVED",      /* string for reserved msg*/
    "QATMGR_MSGTYPE_INSTANCE_INFO", /* string for instance info msg*/
    "QATMGR_MSGTYPE_INSTANCE_NAME", /* string for instance name msg*/
    "QATMGR_MSGTYPE_VFIO_FILE",     /* string for vfio file path msg*/
    "QATMGR_MSGTYPE_NUM_PF_DEVS  ", /* string for pf number msg*/
    "QATMGR_MSGTYPE_PF_DEV_INFO",   /* string for pf device info msg*/
};

#define QATMGR_MSGTYPES_STR_MAX                                                \
    (sizeof(qatmgr_msgtype_str) / sizeof(qatmgr_msgtype_str[0]) - 1)

/* Cache of PF capabilities */
struct pf_capabilities
{
    uint32_t pf;
    uint32_t ext_dc_caps;
    uint32_t capabilities;
    uint32_t ring_to_svc_map;
    struct pf_capabilities *next;
};

static struct pf_capabilities *pf_capabilities_head = NULL;

static struct pf_capabilities *find_pf_capabilities(uint32_t pf)
{
    struct pf_capabilities *current = pf_capabilities_head;
    while (current)
    {
        if (current->pf == pf)
            return current;

        current = current->next;
    }

    return NULL;
}

static void add_pf_capabilities(struct pf_capabilities *caps)
{
    caps->next = pf_capabilities_head;
    pf_capabilities_head = caps;
}

static void cleanup_capabilities_cache()
{
    struct pf_capabilities *current = pf_capabilities_head;
    struct pf_capabilities *next;

    while (current)
    {
        next = current->next;
        free(current);
        current = next;
    }

    pf_capabilities_head = NULL;
}

static int is_qat_device(int device_id)
{
    switch (device_id)
    {
        case QAT_4XXXVF_DEVICE_ID:
        case QAT_401XXVF_DEVICE_ID:
        case QAT_402XXVF_DEVICE_ID:
        case QAT_420XXVF_DEVICE_ID:
            return 1;
        default:
            return 0;
    }
    return 0;
}

static int qat_device_type(int device_id)
{
    switch (device_id)
    {
        case QAT_4XXXVF_DEVICE_ID:
        case QAT_401XXVF_DEVICE_ID:
        case QAT_402XXVF_DEVICE_ID:
            return DEVICE_4XXXVF;
        case QAT_420XXVF_DEVICE_ID:
            return DEVICE_420XXVF;
        default:
            return 0;
    }
    return 0;
}

static char *qat_device_name(int device_id)
{
    switch (device_id)
    {
        case QAT_4XXXVF_DEVICE_ID:
            return "4xxxvf";
        case QAT_401XXVF_DEVICE_ID:
            return "401xxvf";
        case QAT_402XXVF_DEVICE_ID:
            return "402xxvf";
        case QAT_420XXVF_DEVICE_ID:
            return "420xxvf";
        default:
            return "unknown";
    }
}


int init_section_data_mutex()
{
    if (pthread_mutex_init(&section_data_mutex, NULL) != 0)
    {
        return -1;
    }

    return 0;
}

int destroy_section_data_mutex()
{
    if (pthread_mutex_destroy(&section_data_mutex))
    {
        return -1;
    }

    return 0;
}

static void free_cpu_data()
{
    int i;

    if (cpu_data)
    {
        for (i = 0; i < num_nodes; i++)
        {
            if (cpu_data[i].cpu)
            {
                free(cpu_data[i].cpu);
                cpu_data[i].cpu = NULL;
                cpu_data[i].idx = 0;
                cpu_data[i].cores_in_node = 0;
            }
        }
        free(cpu_data);
        cpu_data = NULL;
        num_nodes = 0;
    }
}

void qat_mgr_cleanup_cfg(void)
{
    /*
        Allocated memory:
        section data[num_section_data - 1]
        section_data[i].device_data[section_data[i].num_devices - 1]
        section_data[i].device_data[j].xx_instance_data
    */
    struct qatmgr_section_data *section;
    struct qatmgr_device_data *device;
    int i, j;

    if (section_data)
    {
        section = section_data;
        for (i = 0; i < num_section_data; i++, section++)
        {
            device = section->device_data;

            if (!device)
                continue;

            for (j = 0; j < section->num_devices; j++, device++)
            {
                if (device->dc_instance_data)
                {
                    free(device->dc_instance_data);
                    device->dc_instance_data = NULL;
                }

                if (device->cy_instance_data)
                {
                    free(device->cy_instance_data);
                    device->cy_instance_data = NULL;
                }
            }

            free(section_data[i].device_data);
            section_data[i].device_data = NULL;
        }

        free(section_data);
        section_data = NULL;
        num_section_data = 0;
    }

    free_cpu_data();

    cleanup_capabilities_cache();
}

/*
 * Instead of the default sort order
 *  3d:01.0, 3d:01.1, ... 3d:02.7, 3f:01.0 ... 3f:02.7, da:01.0 ... da:02.7
 * we want all the equivalent device/function entries for each of the PF
 * devices together to facilitate getting devices for policy 0.
 *  3d:01.0, 3f:01.0, da:01.0, 3d:01.1 ...
 */
static int bdf_compare(const void *a, const void *b)
{
    struct qatmgr_dev_data *dev_a = (struct qatmgr_dev_data *)a;
    struct qatmgr_dev_data *dev_b = (struct qatmgr_dev_data *)b;

    if (BDF_FUN(dev_a->bdf) < BDF_FUN(dev_b->bdf))
        return -1;
    if (BDF_FUN(dev_a->bdf) > BDF_FUN(dev_b->bdf))
        return 1;
    if (BDF_DEV(dev_a->bdf) < BDF_DEV(dev_b->bdf))
        return -1;
    if (BDF_DEV(dev_a->bdf) > BDF_DEV(dev_b->bdf))
        return 1;
    if (BDF_BUS(dev_a->bdf) > BDF_BUS(dev_b->bdf))
        return 1;
    if (BDF_BUS(dev_a->bdf) < BDF_BUS(dev_b->bdf))
        return -1;
    return 0;
}

static int open_file_with_link_check(const char *filename, int flags)
{
    int fd;
    struct stat buf;

    fd = open(filename, flags | O_NOFOLLOW);
    if (fd < 0)
    {
        qat_log(LOG_LEVEL_INFO, "Open failed on %s\n", filename);
        return fd;
    }

    if (0 != fstat(fd, &buf))
    {
        qat_log(LOG_LEVEL_ERROR, "Stat failed on %s\n", filename);
        close(fd);
        fd = -1;
        return fd;
    }

    if (buf.st_nlink > 1)
    {
        qat_log(LOG_LEVEL_ERROR, "Detected hardlink for %s\n", filename);
        close(fd);
        fd = -1;
        return fd;
    }

    return fd;
}

static DIR *open_dir_with_link_check(const char *dirname)
{
    int fd;
    DIR *dir;

    fd = open(dirname, O_RDONLY | O_NOFOLLOW | O_DIRECTORY);
    if (fd < 0)
    {
        qat_log(LOG_LEVEL_ERROR, "Cannot open %s\n", dirname);
        return NULL;
    }

    dir = fdopendir(fd);
    if (NULL == dir)
    {
        close(fd);
        qat_log(LOG_LEVEL_ERROR, "Cannot open %s\n", dirname);
        return NULL;
    }

    return dir;
}

int qat_mgr_get_dev_list(unsigned *num_devices,
                         struct qatmgr_dev_data *dev_list,
                         const unsigned list_size,
                         int keep_fd)
{
    DIR *devvfio_dir;
    DIR *sysdevice_dir;
    FILE *sysfile;
    int sysfile_fd;
    struct dirent *vfio_entry;
    struct dirent *device_entry;
    int num_devs = 0;
    unsigned device, vendor;
    char filename[256];
    char devices_dir_name[256];
    int vfiofile = -1;
    char *bdfname;
    unsigned domain, bus, dev, func;
    int found = 0;
    int numa_node;

    if (!dev_list || !list_size || !num_devices)
        return -EINVAL;

    *num_devices = 0;

    devvfio_dir = open_dir_with_link_check(DEVVFIO_DIR);
    if (devvfio_dir == NULL)
    {
        return -EIO;
    }

    /* For each <group> entry in /dev/vfio/ */
    while ((vfio_entry = readdir(devvfio_dir)) != NULL)
    {
        if (vfio_entry->d_name[0] == '.')
            continue;

        /* /dev/vfio/vfio is special entry, should be skipped */
        if (!strncmp(vfio_entry->d_name, VFIO_ENTRY, strlen(VFIO_ENTRY)))
            continue;

        /*
         * A vfio device may be visible but not available.
         * It might, for example, be assigned to a virtual machine.
         * Alternatively, it could be used by another process using
         * a static configuration.
         * In either case, if the device cannot be opened, it should be
         * excluded from the list.
         */
        if (snprintf(filename,
                     sizeof(filename),
                     DEVVFIO_DIR "/%s",
                     vfio_entry->d_name) >= sizeof(filename))
        {
            qat_log(LOG_LEVEL_ERROR, "Filename %s truncated\n", filename);
            continue;
        }

        vfiofile = open_file_with_link_check(filename, O_RDWR);
        if (vfiofile < 0)
            continue;

        if (!keep_fd)
        {
            close(vfiofile);
            vfiofile = -1;
        }

        /* open dir /sys/kernel/iommu_groups/<group>/devices/ */
        if (snprintf(devices_dir_name,
                     sizeof(devices_dir_name),
                     IOMMUGROUP_DEV_DIR,
                     vfio_entry->d_name) >= sizeof(devices_dir_name))
        {
            qat_log(LOG_LEVEL_ERROR, "Filename truncated\n");
            if (vfiofile != -1)
            {
                close(vfiofile);
                vfiofile = -1;
            }
            continue;
        }
        sysdevice_dir = open_dir_with_link_check(devices_dir_name);
        if (sysdevice_dir == NULL)
        {
            if (vfiofile != -1)
            {
                close(vfiofile);
                vfiofile = -1;
            }
            continue;
        }

        found = 0;
        /* For each device in this group. Should only be one. */
        while ((device_entry = readdir(sysdevice_dir)) != NULL)
        {
            if (device_entry->d_name[0] == '.')
                continue;

            /* Open /sys/kernel/iommu_groups/<group>/devices/<device>/device */
            if (snprintf(filename,
                         sizeof(filename),
                         DEVICE_FILE,
                         vfio_entry->d_name,
                         device_entry->d_name) >= sizeof(filename))
            {
                qat_log(LOG_LEVEL_ERROR, "Filename truncated\n");
                break;
            }

            sysfile_fd = open_file_with_link_check(filename, O_RDONLY);
            if (sysfile_fd < 0)
                break;

            sysfile = fdopen(sysfile_fd, "r");
            if (!sysfile)
            {
                close(sysfile_fd);
                break;
            }
            device = 0;
            if (fscanf(sysfile, "%x", &device) != 1)
            {
                qat_log(LOG_LEVEL_INFO,
                        "Failed to read device from %s\n",
                        filename);
                /*
                 * If the fscanf fails, the check of device ids below will fail
                 * and break out of the loop at that point.
                 */
            }
            fclose(sysfile);
            qat_log(LOG_LEVEL_INFO, "Checking %s\n", filename);
            if (!is_qat_device(device))
                break;

            if (snprintf(filename,
                         sizeof(filename),
                         VENDOR_FILE,
                         vfio_entry->d_name,
                         device_entry->d_name) >= sizeof(filename))
            {
                qat_log(LOG_LEVEL_ERROR, "Filename truncated\n");
                break;
            }

            sysfile_fd = open_file_with_link_check(filename, O_RDONLY);
            if (sysfile_fd < 0)
                break;

            sysfile = fdopen(sysfile_fd, "r");
            if (!sysfile)
            {
                qat_log(LOG_LEVEL_ERROR, "Failed to open %s\n", filename);
                close(sysfile_fd);
                break;
            }
            vendor = 0;
            if (fscanf(sysfile, "%x", &vendor) != 1)
            {
                qat_log(LOG_LEVEL_ERROR,
                        "Failed to read vendor from %s\n",
                        filename);
                /*
                 * If the fscanf fails, the check of vendor id below will fail
                 * and break out of the loop at that point.
                 */
            }
            fclose(sysfile);
            if (vendor != INTEL_VENDOR_ID)
                break;

            /* Extract the BDF from the file name */
            bdfname = basename(device_entry->d_name);
            if (sscanf(bdfname, "%x:%x:%x.%x", &domain, &bus, &dev, &func) != 4)
            {
                qat_log(LOG_LEVEL_ERROR, "Failed to scan BDF string\n");
                break;
            }
            dev_list[num_devs].bdf = (domain << 16) + ((0xFF & bus) << 8) +
                                     ((0x1F & dev) << 3) + (0x07 & func);
            if (snprintf(dev_list[num_devs].vfio_file,
                         sizeof(dev_list[num_devs].vfio_file),
                         DEVVFIO_DIR "/%s",
                         vfio_entry->d_name) >=
                sizeof(dev_list[num_devs].vfio_file))
            {
                qat_log(LOG_LEVEL_ERROR, "Filename truncated\n");
                break;
            }

            if (readdir(sysdevice_dir))
            {
                qat_log(LOG_LEVEL_INFO,
                        "Multiple vfio devices in group %s. Ignored\n",
                        vfio_entry->d_name);
                break;
            }

            if (snprintf(filename,
                         sizeof(filename),
                         NUMA_NODE,
                         vfio_entry->d_name,
                         device_entry->d_name) >= sizeof(filename))
            {
                qat_log(LOG_LEVEL_ERROR, "Filename truncated\n");
                break;
            }

            sysfile_fd = open_file_with_link_check(filename, O_RDONLY);
            if (sysfile_fd < 0)
                break;

            sysfile = fdopen(sysfile_fd, "r");
            if (!sysfile)
            {
                qat_log(LOG_LEVEL_ERROR, "Failed to open %s\n", filename);
                close(sysfile_fd);
                break;
            }
            numa_node = 0;
            if (fscanf(sysfile, "%d", &numa_node) != 1)
            {
                qat_log(LOG_LEVEL_ERROR,
                        "Failed to read numa node from %s\n",
                        filename);
            }
            fclose(sysfile);
            /* numa_node may be reported as -1 on VM */
            if (numa_node < 0)
                numa_node = 0;

            dev_list[num_devs].numa_node = numa_node;

            found = 1;

            dev_list[num_devs].devid = device;

            if (keep_fd)
                dev_list[num_devs].group_fd = vfiofile;
            else
                dev_list[num_devs].group_fd = -1;

            num_devs++;
            break;
        }

        if (!found && vfiofile != -1)
        {
            close(vfiofile);
            vfiofile = -1;
        }

        closedir(sysdevice_dir);
        if (num_devs >= list_size)
            break;
    }
    closedir(devvfio_dir);

    *num_devices = num_devs;

    if (!num_devs)
        qat_log(LOG_LEVEL_ERROR, "No devices found\n");

    qsort(dev_list, *num_devices, sizeof(dev_list[0]), bdf_compare);

    return 0;
}

static int qat_mgr_get_device_capabilities(
    struct qatmgr_device_data *device_data,
    int dev_id,
    bool *compatible,
    uint32_t *ext_dc_caps,
    uint32_t *capabilities,
    uint32_t *ring_to_svc_map)
{
    int ret;
    vfio_dev_info_t vfio_dev;
    *compatible = CPA_TRUE;
    ret = open_vfio_dev(device_data->device_file,
                        device_data->device_id,
                        device_data->group_fd,
                        dev_id,
                        &vfio_dev);
    if (ret)
    {
        qat_log(LOG_LEVEL_ERROR, "Cannot open vfio device\n");
        return ret;
    }

    ret = adf_vf2pf_check_compat_version(&vfio_dev.pfvf);
    if (ret)
    {
        close_vfio_dev(&vfio_dev);
        device_data->group_fd = -1;
        if (adf_vf2pf_available())
        {
            qat_log(LOG_LEVEL_ERROR, "Comms incompatible between VF and PF\n");
            *compatible = CPA_FALSE;
        }
        return ret;
    }

    ret = adf_vf2pf_get_ring_to_svc(&vfio_dev.pfvf);
    if (ret)
    {
        qat_log(LOG_LEVEL_ERROR, "Cannot query device ring to service map\n");
        close_vfio_dev(&vfio_dev);
        device_data->group_fd = -1;
        return ret;
    }

    ret = adf_vf2pf_get_capabilities(&vfio_dev.pfvf);
    if (ret)
    {
        qat_log(LOG_LEVEL_ERROR, "Cannot query device capabilities\n");
        close_vfio_dev(&vfio_dev);
        device_data->group_fd = -1;
        return ret;
    }

    *ext_dc_caps = vfio_dev.pfvf.ext_dc_caps;
    *capabilities = vfio_dev.pfvf.capabilities;
    *ring_to_svc_map = vfio_dev.pfvf.ring_to_svc_map;

    close_vfio_dev(&vfio_dev);
    device_data->group_fd = -1;
    return 0;
}

/*
    Calculate bank number for different device configurations.
    Note, this depends on corresponding mapping done by kernel driver.
*/
static int calculate_bank_number(const enum cfg_service_type instance_service,
                                 const int inst_idx,
                                 const uint32_t ring_to_svc_map)
{
    int i, serv_type, serv_found = 0;

    /* Search for the matching service type in ring_to_svc_map */
    for (i = 0; i < INSTANCES_PER_DEVICE; i++)
    {
        serv_type = (ring_to_svc_map >> (i * RING_PAIR_SHIFT)) & SVC_MASK;
        if (instance_service == serv_type)
        {
            if (serv_found == inst_idx)
            {
                return i;
            }
            serv_found++;
        }
    }

    return -1;
}

static uint16_t bdf_pf(const unsigned vf_bdf)
{
    uint16_t pf_bdf = 0;
    unsigned int domain, bus, dev, func;
    FILE *fp = NULL;
    char dev_path[QATMGR_MAX_STRLEN] = { '\0' };
    char dev_info[QATMGR_MAX_STRLEN] = { '\0' };
    char pci_slot_name[DEVICE_NAME_SIZE] = { '\0' };

    snprintf(dev_path,
             sizeof(dev_path),
             "%s/%04x:%02x:%02x.%1x/%s",
             SYSFS_VF_DIR,
             BDF_DOMAIN(vf_bdf),
             BDF_BUS(vf_bdf),
             BDF_DEV(vf_bdf),
             BDF_FUN(vf_bdf),
             SYSFS_VF_UEVENT);

    fp = fopen(dev_path, "r");
    if (fp == NULL)
    {
        qat_log(
            LOG_LEVEL_ERROR, "Failed to open VF sysfs file : %s\n", dev_path);
        return 0;
    }

    while (fgets(dev_info, sizeof(dev_info), fp) != NULL)
    {
        if (strstr(dev_info, PCI_DEV_SLOT_NAME) != NULL)
        {
            sscanf(dev_info, PF_DEVICE_FORMAT, pci_slot_name);
            sscanf(pci_slot_name, "%x:%x:%x.%x", &domain, &bus, &dev, &func);
            pf_bdf = ((0xFF & bus) << 8) + ((0x1F & dev) << 3) + (0x07 & func);
            break;
        }
    }
    fclose(fp);
    return pf_bdf;
}

/**
 * Search for PF index from pf_info data for given vf_bdf
 * returns 0 on success and -1 on failure. If pf_info array
 * is empty (if qatlib is running inside VM) then assigns max value of pkg_id
 * and returns 0
 */
static int get_pkg_id(unsigned vf_bdf, int32_t *vf_pkg_id)
{
    int32_t pkg_id = 0;
    uint16_t pf_bdf = 0;
    uint16_t domain;

    if (!num_pfs)
    {
        *vf_pkg_id = VM_PACKAGE_ID_NONE;
        return 0;
    }

    /* get PF BDF id using VF BDF id */
    pf_bdf = bdf_pf(vf_bdf);
    if (!pf_bdf)
        return -1;
    domain = BDF_DOMAIN(vf_bdf);

    for (pkg_id = 0; pkg_id < num_pfs; pkg_id++)
    {
        if (pf_data[pkg_id].bdf == pf_bdf && pf_data[pkg_id].domain == domain)
        {
            *vf_pkg_id = pkg_id;
            return 0;
        }
    }

    return -1;
}

static int init_cpu_node(int node)
{
    int i = 0;
    for (i = 0; i < num_cpus; i++)
    {
        cpu_data[node].cpu[i] = i;
    }

    cpu_data[node].cores_in_node = num_cpus;
    cpu_data[node].idx = 0;

    return 0;
}

static int init_cpu_node_numa(int node)
{
    int i = 0;
    int j = 0;
    int err = 0;
    struct bitmask *cpus;

    cpus = numa_allocate_cpumask();
    if (!cpus)
    {
        return -1;
    }

    err = numa_node_to_cpus(node, cpus);
    if (err)
    {
        numa_bitmask_free(cpus);
        return -1;
    }

    for (i = 0; i < cpus->size; i++)
    {
        if (numa_bitmask_isbitset(cpus, i))
        {
            cpu_data[node].cpu[j++] = i;
        }
    }

    cpu_data[node].cores_in_node = j;
    cpu_data[node].idx = 0;

    numa_bitmask_free(cpus);

    return 0;
}

static int init_cpu_node_data(int node)
{
    int ret;

    cpu_data[node].cpu = calloc(num_cpus, sizeof(int));

    if (!cpu_data[node].cpu)
    {
        return -ENOMEM;
    }

    if (num_nodes > 1)
    {
        ret = init_cpu_node_numa(node);
    }
    else
    {
        ret = init_cpu_node(node);
    }

    return ret;
}

/**
 * Get next available cpu for given node.
 */
int get_core_affinity(int node)
{
    int cpu = 0;
    int index = 0;
    int cores_in_node = 0;

    index = cpu_data[node].idx;
    cores_in_node = cpu_data[node].cores_in_node;
    cpu = cpu_data[node].cpu[index];

    cpu_data[node].idx = (index + 1) % cores_in_node;

    return cpu;
}

static int init_cpu_data()
{
    int ret = 0;
    int i;

    num_cpus = get_nprocs();

    if (numa_available() < 0)
    {
        num_nodes = 1;
        qat_log(LOG_LEVEL_DEBUG, "No NUMA nodes detected.\n");
    }
    else
    {
        num_nodes = numa_max_node() + 1;
        qat_log(LOG_LEVEL_DEBUG, "Detected %d NUMA nodes.\n", num_nodes);
    }

    cpu_data = calloc(num_nodes, sizeof(struct qatmgr_cpu_data));
    if (!cpu_data)
    {
        qat_log(LOG_LEVEL_ERROR, "Unable to allocate cpu mapping data.\n");
        return -ENOMEM;
    }

    for (i = 0; i < num_nodes; i++)
    {
        ret = init_cpu_node_data(i);
        if (ret)
        {
            qat_log(LOG_LEVEL_ERROR,
                    "Unable to initialize cpu mapping data.\n");
            free_cpu_data();
            return -EAGAIN;
        }
    }
    return 0;
}

static int get_num_instances(struct qatmgr_device_data *device,
                             const unsigned devid,
                             const uint32_t ring_to_svc_map)
{
    int serv_type, i;
    for (i = 0; i < INSTANCES_PER_DEVICE; i++)
    {
        serv_type = (ring_to_svc_map >> (i * RING_PAIR_SHIFT)) & SVC_MASK;
        switch (serv_type)
        {
            case SYM:
                if (device->accel_capabilities &
                    ICP_ACCEL_CAPABILITIES_CRYPTO_SYMMETRIC)
                    device->num_sym_inst++;
                break;
            case ASYM:
                if (device->accel_capabilities &
                    ICP_ACCEL_CAPABILITIES_CRYPTO_ASYMMETRIC)
                    device->num_asym_inst++;
                break;
            case COMP:
                if (device->accel_capabilities &
                    ICP_ACCEL_CAPABILITIES_COMPRESSION)
                    device->num_dc_inst++;
                break;
            default:
                return -1;
        }
    }


    if (device->num_sym_inst == INSTANCES_PER_DEVICE ||
        device->num_asym_inst == INSTANCES_PER_DEVICE)
        device->num_cy_inst = INSTANCES_PER_DEVICE;
    else if (device->num_sym_inst == 2 || device->num_asym_inst == 2)
        device->num_cy_inst = INSTANCES_PER_DEVICE / 2;

    return 0;
}

int qat_mgr_build_data(const struct qatmgr_dev_data dev_list[],
                       const int num_vf_devices,
                       int policy,
                       int static_cfg)
{
    int i, j, k;
    struct qatmgr_section_data *section;
    struct qatmgr_instance_data *dc_inst;
    struct qatmgr_cy_instance_data *cy_inst;
    struct qatmgr_device_data *device_data;
    int num_vf_groups;
    int vf_idx = 0;
    int num_vfs_this_section;
    int pf = 0;
    unsigned devid;
    char pf_str[10];
    ENTRY pf_entry = { pf_str, NULL };
    int pfs_per_vf_group[ADF_MAX_DEVICES] = { 0 };
    uint32_t ext_dc_caps, capabilities;
    uint32_t ring_to_svc_map = DEFAULT_RING_TO_SRV_MAP;
    bool compatible;
    bool vm = false;
    int ret;
    struct pf_capabilities *cached_capabilities;
    int section_num_sym_inst = 0;
    int section_num_asym_inst = 0;
    int section_num_dc_inst = 0;
    int32_t vf_pkg_id = 0;

    if (!num_vf_devices)
        return -EINVAL;

    if (num_pfs == PF_INFO_UNINITIALISED)
    {
        num_pfs = adf_vfio_init_pfs_info(pf_data,
                                         sizeof(pf_data) / sizeof(pf_data[0]));
    }

    if (num_pfs < 0 || num_pfs > ADF_MAX_PF_DEVICES)
    {
        qat_log(LOG_LEVEL_ERROR, "Invalid number Pfs\n");
        return -1;
    }

    if (!num_pfs)
    {
        vm = true;
        qat_log(LOG_LEVEL_DEBUG,
                "Unable to find pfs in the system, assuming "
                "qat_mgr_lib is running inside VM\n");
    }

    ret = init_cpu_data();
    if (ret)
    {
        return ret;
    }

    /*
     * A VF group is a set of VFs with the same device/function
     * but from different PFs.
     * The dev_list is sorted so that each VF in a group are consecutive.
     * We know we have a new group when we find a PF that already exists in
     * the first group.
     */
    if (!static_cfg)
    {
        num_vf_groups = 1;

        /* Create hash table for mapping devices */
        if (hcreate(ADF_MAX_DEVICES) == 0)
        {
            qat_log(LOG_LEVEL_ERROR, "Error while creating hash table\n");
            free_cpu_data();
            return -ENOMEM;
        }

        /* Count VF groups */
        for (i = 0; i < num_vf_devices; i++)
        {
            /* Convert PF address to int - take node into account */
            pf = PF(dev_list[i].bdf);
            /* Convert address to string to use as hash table key */
            snprintf(pf_str, sizeof(pf_str), "%d", pf);
            /* Check if pf is already in the hash table */
            if (hsearch(pf_entry, FIND) != NULL)
            {
                /* Device already in hash table - increment vf groups */
                num_vf_groups++;
                /* Need to create new hash table */
                hdestroy();
                if (hcreate(ADF_MAX_DEVICES) == 0)
                {
                    qat_log(LOG_LEVEL_ERROR,
                            "Error while creating hash table\n");
                    free_cpu_data();
                    return -ENOMEM;
                }
            }
            pfs_per_vf_group[num_vf_groups - 1]++;

            /* Insert device to hash table */
            if (hsearch(pf_entry, ENTER) == NULL)
            {
                qat_log(LOG_LEVEL_ERROR, "No space left in hash table\n");
                free_cpu_data();
                return -ENOMEM;
            }
        }
        qat_log(LOG_LEVEL_DEBUG, "num_vf_groups %d\n", num_vf_groups);
        /* Destroy hash table */
        hdestroy();

        /*
         * For policy 0, each process will get a VF from each PF so there can
         * be a max of num_vf_groups processes.
         * For policy <n>, each process will get <n> VFs so there can be
         * a max of num_vf_devices / <n> processes.
         */
        if (policy == 0)
        {
            num_section_data = num_vf_groups;
        }
        else
        {
            num_section_data = num_vf_devices / policy;
        }

        if (num_section_data <= 0)
        {
            qat_log(LOG_LEVEL_ERROR,
                    "Policy %d is greater than the number of "
                    "available devices %d\n",
                    policy,
                    num_vf_devices);
            free_cpu_data();
            return -EINVAL;
        }
        qat_log(LOG_LEVEL_DEBUG, "num_section_data %d\n", num_section_data);
    }
    else if (num_vf_devices >= policy)
    {
        num_section_data = 1;
    }
    else
    {
        qat_log(LOG_LEVEL_ERROR,
                "Policy %d is incompatible with the number of "
                "available devices %d\n",
                policy,
                num_vf_devices);
        free_cpu_data();
        return -EINVAL;
    }

    section_data = calloc(num_section_data, sizeof(struct qatmgr_section_data));
    if (!section_data)
    {
        qat_log(LOG_LEVEL_ERROR, "Malloc failed for section data\n");
        num_section_data = 0;
        free_cpu_data();
        return -EAGAIN;
    }

    section = section_data;
    for (i = 0; i < num_section_data; i++, section++)
    {
        snprintf(section->section_name,
                 sizeof(section->section_name),
                 "SSL_INT_%d",
                 i);
        snprintf(section->base_name, sizeof(section->base_name), "SSL");
        section->assigned_tid = 0;

        if (policy)
        {
            num_vfs_this_section = policy;
            if (num_vfs_this_section > num_vf_devices - vf_idx)
                num_vfs_this_section = num_vf_devices - vf_idx;
        }
        else
        {
            /*
             * Policy 0, one VF from each different PF.
             * Use cached number of PFs.
             */
            num_vfs_this_section = pfs_per_vf_group[i];
        }
        section->num_devices = num_vfs_this_section;

        /* Create device data */
        section->device_data =
            calloc(num_vfs_this_section, sizeof(struct qatmgr_device_data));
        if (!section->device_data)
        {
            qat_log(LOG_LEVEL_ERROR, "Malloc failed for device data\n");
            qat_mgr_cleanup_cfg();
            return -EAGAIN;
        }

        section_num_sym_inst = 0;
        section_num_asym_inst = 0;
        section_num_dc_inst = 0;

        device_data = section->device_data;
        for (j = 0; j < num_vfs_this_section; j++, device_data++, vf_idx++)
        {
            qat_log(LOG_LEVEL_DEBUG,
                    "section %d, BDF %X\n",
                    i,
                    dev_list[vf_idx].bdf);
            snprintf(device_data->device_id,
                     sizeof(device_data->device_id),
                     "%04x:%02x:%02x.%01x",
                     BDF_DOMAIN(dev_list[vf_idx].bdf),
                     BDF_BUS(dev_list[vf_idx].bdf),
                     BDF_DEV(dev_list[vf_idx].bdf),
                     BDF_FUN(dev_list[vf_idx].bdf));
            snprintf(device_data->device_file,
                     sizeof(device_data->device_file),
                     "%s",
                     dev_list[vf_idx].vfio_file);
            device_data->group_fd = dev_list[vf_idx].group_fd;
            device_data->accelid = j;
            device_data->node = dev_list[vf_idx].numa_node;

            if (get_pkg_id(dev_list[vf_idx].bdf, &vf_pkg_id))
            {
                qat_log(LOG_LEVEL_ERROR,
                        "Failed to find pkg_id for the device\n");
                qat_mgr_cleanup_cfg();
                return -EAGAIN;
            }

            /* since sample code uses package id for gathering info from devices
             * this overrides pkg id to accel id if qatlib is running on VM */
            if (vf_pkg_id == VM_PACKAGE_ID_NONE)
            {
                vf_pkg_id = device_data->accelid;
            }

            device_data->pkg_id = vf_pkg_id;
            devid = dev_list[vf_idx].devid;

                device_data->max_banks = 4;
                device_data->max_rings_per_bank = 2;
                device_data->arb_mask = 0x01;
                device_data->accel_capabilities =
                    ICP_ACCEL_CAPABILITIES_CRYPTO_SYMMETRIC |
                    ICP_ACCEL_CAPABILITIES_CRYPTO_ASYMMETRIC |
                    ICP_ACCEL_CAPABILITIES_CIPHER |
                    ICP_ACCEL_CAPABILITIES_AUTHENTICATION |
                    ICP_ACCEL_CAPABILITIES_CRYPTO_SHA3 |
                    ICP_ACCEL_CAPABILITIES_SHA3_EXT |
                    ICP_ACCEL_CAPABILITIES_HKDF |
                    ICP_ACCEL_CAPABILITIES_ECEDMONT |
                    ICP_ACCEL_CAPABILITIES_CHACHA_POLY |
                    ICP_ACCEL_CAPABILITIES_AESGCM_SPC |
                    ICP_ACCEL_CAPABILITIES_AES_V2;
                device_data->extended_capabilities = 0x0;
            /**
             * Send query to get capabilities from PF.
             * qat_mgr_get_device_capabilities will open device, initialize
             * VF2PF communication, query capabilities and close device.
             *
             * Before first query, we don't know if PF is supporting VF2PF (1st
             * call to adf_vf2pf_available will report availability of VF2PF),
             * in case where PF is not supporting VF2PF, consecutive calls to
             * adf_vf2pf_available will report lack of VF2PF and hardcoded
             * "fallback" capabilities defined above will be used.
             */

            cached_capabilities = NULL;
            if (!vm)
            {
                /**
                 * If running on a host, it can be assumed that all devices with
                 * the same domain+bus are VFs from the same PF and so have the
                 * same capabilities. So it's an optimization to query the
                 * capabilities of only one VF and cache them to populate the
                 * other VFs.
                 */
                pf = PF(dev_list[vf_idx].bdf);
                cached_capabilities = find_pf_capabilities(pf);
            }

            if (cached_capabilities)
            {
                device_data->accel_capabilities =
                    cached_capabilities->capabilities;
                device_data->extended_capabilities =
                    cached_capabilities->ext_dc_caps;
                ring_to_svc_map = cached_capabilities->ring_to_svc_map;
            }
            else if (adf_vf2pf_available())
            {
                ret = qat_mgr_get_device_capabilities(device_data,
                                                      devid,
                                                      &compatible,
                                                      &ext_dc_caps,
                                                      &capabilities,
                                                      &ring_to_svc_map);
                if (0 == ret)
                {
                    /*
                     * Override the ecEdMont capability reported by the kernel.
                     * The reason for this is that some QAT Gen4 kernel drivers
                     * don't report this capability even though it is present
                     * in all devices that have asym.
                     */
                    if (capabilities & ICP_ACCEL_CAPABILITIES_CRYPTO_ASYMMETRIC)
                        capabilities |= ICP_ACCEL_CAPABILITIES_ECEDMONT;

                    device_data->accel_capabilities = capabilities;
                    device_data->extended_capabilities = ext_dc_caps;
                }
                else if (!compatible)
                {
                    qat_log(LOG_LEVEL_ERROR,
                            "Detected not compatible PF driver\n");
                    qat_mgr_cleanup_cfg();
                    return ret;
                }

                if (0 == ret && !vm)
                {
                    cached_capabilities =
                        calloc(1, sizeof(struct pf_capabilities));
                    if (!cached_capabilities)
                    {
                        qat_log(LOG_LEVEL_ERROR,
                                "Malloc failed for capabilities cache\n");
                        qat_mgr_cleanup_cfg();
                        return -EAGAIN;
                    }
                    cached_capabilities->pf = pf;
                    cached_capabilities->capabilities = capabilities;
                    cached_capabilities->ext_dc_caps = ext_dc_caps;
                    cached_capabilities->ring_to_svc_map = ring_to_svc_map;
                    add_pf_capabilities(cached_capabilities);
                }
            }

            snprintf(device_data->name,
                     sizeof(device_data->name),
                     "%s",
                     qat_device_name(devid));
            device_data->device_type = qat_device_type(devid);
            device_data->pci_id = devid;

            /*  1 device has 4 RPs = 4 logical instances.
             *  Available device configurations:
             *   - all sym
             *   - all asym
             *   - all dc
             *   - asym, dc (2/2 instances)
             *   - sym, dc (2/2 instances)
             *   - cy (2 sym/2 asym)
             */
            device_data->services = 0;
            if (device_data->accel_capabilities &
                ICP_ACCEL_CAPABILITIES_CRYPTO_SYMMETRIC)
            {
                device_data->services |= SERV_TYPE_SYM;
            }

            if (device_data->accel_capabilities &
                ICP_ACCEL_CAPABILITIES_CRYPTO_ASYMMETRIC)
            {
                device_data->services |= SERV_TYPE_ASYM;
            }

            if (device_data->accel_capabilities &
                ICP_ACCEL_CAPABILITIES_COMPRESSION)
            {
                device_data->services |= SERV_TYPE_DC;
            }

            /*  populate configuration for a device
             *   first determine num instances per device
             */
            ret = get_num_instances(device_data, devid, ring_to_svc_map);
            if (ret)
            {
                qat_log(LOG_LEVEL_ERROR, "Detected unknown service\n");
                qat_mgr_cleanup_cfg();
                return -1;
            }

            if (device_data->num_dc_inst)
            {
                /* Create instance data */
                device_data->dc_instance_data =
                    calloc(device_data->num_dc_inst,
                           sizeof(struct qatmgr_instance_data));
                if (!device_data->dc_instance_data)
                {
                    qat_log(LOG_LEVEL_ERROR,
                            "Malloc failed for dc instance data\n");
                    qat_mgr_cleanup_cfg();
                    return -EAGAIN;
                }
            }

            /* SYM and ASYM are stored inside CY instance data */
            if (device_data->num_cy_inst)
            {
                device_data->cy_instance_data =
                    calloc(device_data->num_cy_inst,
                           sizeof(struct qatmgr_cy_instance_data));
                if (!device_data->cy_instance_data)
                {
                    qat_log(LOG_LEVEL_ERROR,
                            "Malloc failed for cy instance data\n");
                    qat_mgr_cleanup_cfg();
                    return -EAGAIN;
                }
            }

            /* populate instance data */
            cy_inst = device_data->cy_instance_data;
            for (k = 0; k < device_data->num_asym_inst; k++, cy_inst++)
            {
                snprintf(cy_inst->asym.name,
                         sizeof(cy_inst->asym.name),
                         "asym%d",
                         section_num_asym_inst++);
                cy_inst->asym.accelid = device_data->accelid;
                cy_inst->asym.service_type = SERV_TYPE_ASYM;

                    cy_inst->asym.bank_number =
                        calculate_bank_number(ASYM, k, ring_to_svc_map);
                    if (cy_inst->asym.bank_number < 0)
                    {
                        qat_log(LOG_LEVEL_ERROR,
                                "Cannot find bank number for asym instance\n");
                        qat_mgr_cleanup_cfg();
                        return -1;
                    }
                    cy_inst->asym.ring_tx = 0;
                    cy_inst->asym.ring_rx = 1;
                cy_inst->asym.is_polled = 1;
                cy_inst->asym.num_concurrent_requests = 64;
                cy_inst->asym.core_affinity =
                    get_core_affinity(device_data->node);
            }

            cy_inst = device_data->cy_instance_data;
            for (k = 0; k < device_data->num_sym_inst; k++, cy_inst++)
            {
                snprintf(cy_inst->sym.name,
                         sizeof(cy_inst->sym.name),
                         "sym%d",
                         section_num_sym_inst++);
                cy_inst->sym.accelid = device_data->accelid;
                cy_inst->sym.service_type = SERV_TYPE_SYM;

                    cy_inst->sym.bank_number =
                        calculate_bank_number(SYM, k, ring_to_svc_map);
                    if (cy_inst->sym.bank_number < 0)
                    {
                        qat_log(LOG_LEVEL_ERROR,
                                "Cannot find bank number for sym instance\n");
                        qat_mgr_cleanup_cfg();
                        return -1;
                    }
                    cy_inst->sym.ring_tx = 0;
                    cy_inst->sym.ring_rx = 1;
                cy_inst->sym.is_polled = 1;
                cy_inst->sym.num_concurrent_requests = 512;
                cy_inst->sym.core_affinity =
                    get_core_affinity(device_data->node);
            }

            dc_inst = device_data->dc_instance_data;
            for (k = 0; k < device_data->num_dc_inst; k++, dc_inst++)
            {
                snprintf(dc_inst->name,
                         sizeof(dc_inst->name),
                         "dc%d",
                         section_num_dc_inst++);
                dc_inst->accelid = device_data->accelid;
                dc_inst->service_type = SERV_TYPE_DC;

                    dc_inst->bank_number =
                        calculate_bank_number(COMP, k, ring_to_svc_map);
                    if (dc_inst->bank_number < 0)
                    {
                        qat_log(LOG_LEVEL_ERROR,
                                "Cannot find bank number for dc instance\n");
                        qat_mgr_cleanup_cfg();
                        return -1;
                    }
                    dc_inst->ring_tx = 0;
                    dc_inst->ring_rx = 1;
                dc_inst->is_polled = 1;
                dc_inst->num_concurrent_requests = 512;
                dc_inst->core_affinity = get_core_affinity(device_data->node);
            }
        }
    }

    return 0;
}

bool qat_mgr_is_dev_available()
{
    DIR *devvfio_dir;
    DIR *sysdevice_dir;
    struct dirent *vfio_entry;
    struct dirent *device_entry;
    FILE *sysfile;
    int sysfile_fd;
    char devices_dir_name[256];
    bool dev_found = false;
    char filename[256];
    unsigned int device;

    devvfio_dir = open_dir_with_link_check(DEVVFIO_DIR);
    if (devvfio_dir == NULL)
    {
        return false;
    }

    /* For each <group> entry in /dev/vfio/ */
    while ((vfio_entry = readdir(devvfio_dir)) != NULL)
    {
        /* If any QAT device was found, quit immediately */
        if (dev_found)
            break;

        if (vfio_entry->d_name[0] == '.')
            continue;

        /* /dev/vfio/vfio is special entry, should be skipped */
        if (!strncmp(vfio_entry->d_name, VFIO_ENTRY, strlen(VFIO_ENTRY)))
            continue;

        /* open dir /sys/kernel/iommu_groups/<group>/devices/ */
        if (snprintf(devices_dir_name,
                     sizeof(devices_dir_name),
                     IOMMUGROUP_DEV_DIR,
                     vfio_entry->d_name) >= sizeof(devices_dir_name))
        {
            qat_log(LOG_LEVEL_ERROR, "Filename truncated\n");
            continue;
        }

        sysdevice_dir = open_dir_with_link_check(devices_dir_name);
        if (sysdevice_dir == NULL)
        {
            continue;
        }

        /* For each device in this group. Should only be one. */
        while ((device_entry = readdir(sysdevice_dir)) != NULL)
        {
            if (device_entry->d_name[0] == '.')
                continue;

            /* Open /sys/kernel/iommu_groups/<group>/devices/<device>/device */
            if (snprintf(filename,
                         sizeof(filename),
                         DEVICE_FILE,
                         vfio_entry->d_name,
                         device_entry->d_name) >= sizeof(filename))
            {
                qat_log(LOG_LEVEL_ERROR, "Filename truncated\n");
                break;
            }

            sysfile_fd = open_file_with_link_check(filename, O_RDONLY);
            if (sysfile_fd < 0)
                break;

            sysfile = fdopen(sysfile_fd, "r");
            if (!sysfile)
            {
                close(sysfile_fd);
                break;
            }
            device = 0;
            if (fscanf(sysfile, "%x", &device) != 1)
            {
                qat_log(LOG_LEVEL_INFO,
                        "Failed to read device from %s\n",
                        filename);
                /*
                 * If the fscanf fails, the check of device ids below will fail
                 * and we will check next dev.
                 */
            }
            fclose(sysfile);
            qat_log(LOG_LEVEL_INFO, "Checking %s\n", filename);
            if (is_qat_device(device))
            {
                dev_found = true;
                break;
            }
        }
        closedir(sysdevice_dir);
    }
    closedir(devvfio_dir);

    return dev_found;
}

static void dump_message(void *ptr, char *text)
{
    struct qatmgr_msg_req *req = ptr;
    int payload_size;
    uint8_t *payload;
    int i;

    if (debug_level < 2)
        return;

    ICP_CHECK_FOR_NULL_PARAM_VOID(ptr);
    ICP_CHECK_FOR_NULL_PARAM_VOID(text);

    printf("%s\n", text);
    printf("Message type %hu\n", (unsigned short int)(req->hdr.type));
    if (req->hdr.type > 0 && req->hdr.type <= QATMGR_MSGTYPES_STR_MAX)
        printf("Message name %s\n", qatmgr_msgtype_str[req->hdr.type]);
    printf("   length %d\n", req->hdr.len);
    payload_size = req->hdr.len - sizeof(req->hdr);
    payload = (uint8_t *)req + sizeof(req->hdr);

    if (payload_size > 0 && payload_size <= MAX_PAYLOAD_SIZE)
    {
        printf("    Payload: ");
        for (i = 0; i < payload_size; i++, payload++)
        {
            printf("%02X ", *payload);
            if (i % 16 == 0)
                printf("\n");
        }
        printf("\n");
    }
    else
    {
        qat_log(
            LOG_LEVEL_ERROR,
            "Message payload size (%d) out of range. Max payload size is %lu\n",
            payload_size,
            MAX_PAYLOAD_SIZE);
    }
}

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

static void err_msg(struct qatmgr_msg_rsp *rsp, char *text)
{
    ICP_CHECK_FOR_NULL_PARAM_VOID(rsp);
    ICP_CHECK_FOR_NULL_PARAM_VOID(text);

    rsp->hdr.type = QATMGR_MSGTYPE_BAD;
    rsp->hdr.version = THIS_LIB_VERSION;
    ICP_STRLCPY(rsp->error_text, text, sizeof(rsp->error_text));
    rsp->hdr.len =
        sizeof(rsp->hdr) + ICP_ARRAY_STRLEN_SANITIZE(rsp->error_text) + 1;
}

static void build_msg_header(struct qatmgr_msg_rsp *rsp,
                             int type,
                             int payload_size)
{
    ICP_CHECK_FOR_NULL_PARAM_VOID(rsp);

    rsp->hdr.type = type;
    rsp->hdr.version = THIS_LIB_VERSION;
    rsp->hdr.len = sizeof(rsp->hdr) + payload_size;
}

static int handle_get_num_devices(struct qatmgr_msg_req *req,
                                  struct qatmgr_msg_rsp *rsp,
                                  int index)
{
    struct qatmgr_section_data *section;

    ICP_CHECK_FOR_NULL_PARAM(req);
    ICP_CHECK_FOR_NULL_PARAM(rsp);

    if (req->hdr.len != sizeof(req->hdr))
    {
        qat_log(LOG_LEVEL_ERROR, "Bad length\n");
        err_msg(rsp, "Inconsistent length");
        return -1;
    }

    dump_message(req, "Request");

    if (index < 0 || index >= num_section_data)
    {
        qat_log(LOG_LEVEL_ERROR, "Bad index\n");
        err_msg(rsp, "Invalid index");
        return -1;
    }
    section = section_data + index;

    rsp->num_devices = section->num_devices;
    build_msg_header(rsp, QATMGR_MSGTYPE_NUM_DEVICES, sizeof(rsp->num_devices));

    dump_message(rsp, "Response");
    return 0;
}

static int handle_get_device_info(struct qatmgr_msg_req *req,
                                  struct qatmgr_msg_rsp *rsp,
                                  int index)
{
    struct qatmgr_section_data *section;
    unsigned device_num;

    ICP_CHECK_FOR_NULL_PARAM(req);
    ICP_CHECK_FOR_NULL_PARAM(rsp);

    if (req->hdr.len != sizeof(req->hdr) + sizeof(req->device_num))
    {
        qat_log(LOG_LEVEL_ERROR, "Bad length\n");
        err_msg(rsp, "Inconsistent length");
        return -1;
    }

    dump_message(req, "Request");

    if (index < 0 || index >= num_section_data)
    {
        qat_log(LOG_LEVEL_ERROR, "Bad index\n");
        err_msg(rsp, "Invalid index");
        return -1;
    }
    section = section_data + index;

    device_num = req->device_num;
    if (device_num >= section->num_devices)
    {
        qat_log(LOG_LEVEL_ERROR,
                "Invalid device %d >= %d\n",
                device_num,
                section->num_devices);
        err_msg(rsp, "Invalid device number");
        return -1;
    }

    rsp->device_info.device_num = device_num;
    rsp->device_info.device_type = section->device_data[device_num].device_type;
    ICP_STRLCPY(rsp->device_info.device_name,
                section->device_data[device_num].name,
                sizeof(rsp->device_info.device_name));
    rsp->device_info.capability_mask =
        section->device_data[device_num].accel_capabilities;
    rsp->device_info.extended_capabilities =
        section->device_data[device_num].extended_capabilities;
    rsp->device_info.max_banks = section->device_data[device_num].max_banks;
    rsp->device_info.max_rings_per_bank =
        section->device_data[device_num].max_rings_per_bank;
    rsp->device_info.arb_mask = section->device_data[device_num].arb_mask;
    rsp->device_info.services = section->device_data[device_num].services;
    rsp->device_info.pkg_id = section->device_data[device_num].pkg_id;
    rsp->device_info.node_id = section->device_data[device_num].node;
    rsp->device_info.device_pci_id = section->device_data[device_num].pci_id;
    rsp->device_info.num_cy_instances =
        section->device_data[device_num].num_cy_inst;
    rsp->device_info.num_dc_instances =
        section->device_data[device_num].num_dc_inst;
    build_msg_header(rsp, QATMGR_MSGTYPE_DEVICE_INFO, sizeof(rsp->device_info));

    dump_message(rsp, "Response");
    return 0;
}

static int handle_get_device_id(struct qatmgr_msg_req *req,
                                struct qatmgr_msg_rsp *rsp,
                                int index)
{
    struct qatmgr_section_data *section;
    struct qatmgr_device_data *device_data;
    unsigned device_num;

    ICP_CHECK_FOR_NULL_PARAM(req);
    ICP_CHECK_FOR_NULL_PARAM(rsp);

    if (req->hdr.len != sizeof(req->hdr) + sizeof(req->device_num))
    {
        qat_log(LOG_LEVEL_ERROR, "Bad length\n");
        err_msg(rsp, "Inconsistent length");
        return -1;
    }

    dump_message(req, "Request");

    if (index < 0 || index >= num_section_data)
    {
        qat_log(LOG_LEVEL_ERROR, "Bad index\n");
        err_msg(rsp, "Invalid index");
        return -1;
    }
    section = section_data + index;

    device_num = req->device_num;
    if (device_num >= section->num_devices)
    {
        qat_log(LOG_LEVEL_ERROR, "Invalid device %d\n", device_num);
        err_msg(rsp, "Invalid device number");
        return -1;
    }
    device_data = section->device_data;
    device_data += device_num;

    rsp->hdr.type = QATMGR_MSGTYPE_DEVICE_ID;
    rsp->hdr.version = THIS_LIB_VERSION;
    ICP_STRLCPY(rsp->device_id, device_data->device_id, sizeof(rsp->device_id));
    build_msg_header(rsp,
                     QATMGR_MSGTYPE_DEVICE_ID,
                     ICP_ARRAY_STRLEN_SANITIZE(rsp->device_id) + 1);
    dump_message(rsp, "Response");
    return 0;
}

static int handle_get_vfio_name(struct qatmgr_msg_req *req,
                                struct qatmgr_msg_rsp *rsp,
                                int index)
{
    struct qatmgr_section_data *section;
    struct qatmgr_device_data *device_data;
    unsigned device_num;
    size_t len;

    ICP_CHECK_FOR_NULL_PARAM(req);
    ICP_CHECK_FOR_NULL_PARAM(rsp);

    if (req->hdr.len != sizeof(req->hdr) + sizeof(req->device_num))
    {
        qat_log(LOG_LEVEL_ERROR, "Bad length\n");
        err_msg(rsp, "Inconsistent length");
        return -1;
    }

    dump_message(req, "Request");

    if (index < 0 || index >= num_section_data)
    {
        qat_log(LOG_LEVEL_ERROR, "Bad index\n");
        err_msg(rsp, "Invalid index");
        return -1;
    }
    section = section_data + index;

    device_num = req->device_num;
    if (device_num >= section->num_devices)
    {
        qat_log(LOG_LEVEL_ERROR, "Invalid device %d\n", device_num);
        err_msg(rsp, "Invalid device number");
        return -1;
    }
    device_data = section->device_data;
    device_data += device_num;

    rsp->hdr.type = QATMGR_MSGTYPE_VFIO_FILE;
    rsp->hdr.version = THIS_LIB_VERSION;
    rsp->vfio_file.fd = device_data->group_fd;

    ICP_STRLCPY(rsp->vfio_file.name,
                device_data->device_file,
                sizeof(rsp->vfio_file.name));

    len = ICP_ARRAY_STRLEN_SANITIZE(rsp->vfio_file.name);

    build_msg_header(
        rsp, QATMGR_MSGTYPE_VFIO_FILE, sizeof(rsp->vfio_file.fd) + len + 1);

    dump_message(rsp, "Response");
    return 0;
}

static int handle_get_instance_name(struct qatmgr_msg_req *req,
                                    struct qatmgr_msg_rsp *rsp,
                                    int index)
{
    struct qatmgr_section_data *section;
    struct qatmgr_device_data *device;
    int instance_type;
    int instance_num;
    int device_num;
    char *inst_name;

    ICP_CHECK_FOR_NULL_PARAM(req);
    ICP_CHECK_FOR_NULL_PARAM(rsp);

    if (req->hdr.len != sizeof(req->hdr) + sizeof(req->inst))
    {
        qat_log(LOG_LEVEL_ERROR, "Bad length\n");
        err_msg(rsp, "Inconsistent length");
        return -1;
    }

    dump_message(req, "Request");

    if (index < 0 || index >= num_section_data)
    {
        qat_log(LOG_LEVEL_ERROR, "Bad index\n");
        err_msg(rsp, "Invalid index");
        return -1;
    }
    section = section_data + index;

    instance_type = req->inst.type;
    instance_num = req->inst.num;
    device_num = req->inst.device_num;

    device = section->device_data + device_num;

    if (device_num >= section->num_devices)
    {
        qat_log(LOG_LEVEL_ERROR,
                "Invalid device number %d for section %d\n",
                device_num,
                index);
        err_msg(rsp, "Invalid device number");
        return -1;
    }

    if (instance_type == SERV_TYPE_DC)
    {
        if (instance_num >= device->num_dc_inst)
        {
            qat_log(LOG_LEVEL_ERROR,
                    "Bad dc instance number %d for section %d\n",
                    instance_num,
                    index);
            err_msg(rsp, "Invalid DC instance number");
            return -1;
        }
        ICP_STRLCPY(rsp->name,
                    device->dc_instance_data[instance_num].name,
                    sizeof(rsp->name));
        build_msg_header(rsp,
                         QATMGR_MSGTYPE_INSTANCE_NAME,
                         ICP_ARRAY_STRLEN_SANITIZE(rsp->name) + 1);
    }
    else if (instance_type == SERV_TYPE_CY)
    {
        if (instance_num >= device->num_cy_inst)
        {
            qat_log(LOG_LEVEL_ERROR,
                    "Bad cy instance number %d for section %d\n",
                    instance_num,
                    index);
            err_msg(rsp, "Invalid CY instance number");
            return -1;
        }
        /* CYxname SAL string param limitation, for CY only and SYM only
         * cpaCyInstanceGetInfo2 will get same instance names */
        if (device->services == SERV_TYPE_ASYM ||
            device->services == SERV_TYPE_ASYM_DC)
        {
            inst_name = device->cy_instance_data[instance_num].asym.name;
        }
        else
        {
            inst_name = device->cy_instance_data[instance_num].sym.name;
        }
        ICP_STRLCPY(rsp->name, inst_name, sizeof(rsp->name));
        build_msg_header(rsp,
                         QATMGR_MSGTYPE_INSTANCE_NAME,
                         ICP_ARRAY_STRLEN_SANITIZE(rsp->name) + 1);
    }
    else if (instance_type == SERV_TYPE_SYM)
    {
        if (instance_num >= device->num_sym_inst)
        {
            qat_log(LOG_LEVEL_ERROR,
                    "Bad cy.sym instance number %d for section %d\n",
                    instance_num,
                    index);
            err_msg(rsp, "Invalid SYM instance number");
            return -1;
        }
        ICP_STRLCPY(rsp->name,
                    device->cy_instance_data[instance_num].sym.name,
                    sizeof(rsp->name));
        build_msg_header(rsp,
                         QATMGR_MSGTYPE_INSTANCE_NAME,
                         ICP_ARRAY_STRLEN_SANITIZE(rsp->name) + 1);
    }
    else if (instance_type == SERV_TYPE_ASYM)
    {
        if (instance_num >= device->num_asym_inst)
        {
            qat_log(LOG_LEVEL_ERROR,
                    "Bad cy.asym instance number %d for section %d\n",
                    instance_num,
                    index);
            err_msg(rsp, "Invalid ASYM instance number");
            return -1;
        }
        ICP_STRLCPY(rsp->name,
                    device->cy_instance_data[instance_num].asym.name,
                    sizeof(rsp->name));
        build_msg_header(rsp,
                         QATMGR_MSGTYPE_INSTANCE_NAME,
                         ICP_ARRAY_STRLEN_SANITIZE(rsp->name) + 1);
    }
    else
    {
        qat_log(
            LOG_LEVEL_ERROR, "unsupported instance type %d\n", instance_type);
        err_msg(rsp, "Unknown instance type");
        return -1;
    }
    dump_message(rsp, "Response");
    return 0;
}

static int handle_get_instance_info(struct qatmgr_msg_req *req,
                                    struct qatmgr_msg_rsp *rsp,
                                    int index)
{
    struct qatmgr_section_data *section;
    struct qatmgr_device_data *device;
    struct qatmgr_instance_data *instance_data;
    struct qatmgr_cy_instance_data *cy_instance_data;
    int instance_type;
    int instance_num;
    int device_num;

    ICP_CHECK_FOR_NULL_PARAM(req);
    ICP_CHECK_FOR_NULL_PARAM(rsp);

    if (req->hdr.len != sizeof(req->hdr) + sizeof(req->inst))
    {
        qat_log(LOG_LEVEL_ERROR, "Bad length\n");
        err_msg(rsp, "Inconsistent length");
        return -1;
    }

    dump_message(req, "Request");

    if (index < 0 || index >= num_section_data)
    {
        qat_log(LOG_LEVEL_ERROR, "Bad index\n");
        err_msg(rsp, "Invalid index");
        return -1;
    }
    section = section_data + index;

    instance_type = req->inst.type;
    instance_num = req->inst.num;
    device_num = req->inst.device_num;

    device = section->device_data + device_num;

    if (device_num >= section->num_devices)
    {
        qat_log(LOG_LEVEL_ERROR,
                "Invalid device number %d for section %d\n",
                device_num,
                index);
        err_msg(rsp, "Invalid device number");
        return -1;
    }

    if (instance_type == SERV_TYPE_DC)
    {
        if (instance_num >= device->num_dc_inst)
        {
            qat_log(LOG_LEVEL_ERROR,
                    "Bad dc instance number %d for section %d\n",
                    instance_num,
                    index);
            err_msg(rsp, "Invalid DC instance number");
            return -1;
        }
        instance_data = device->dc_instance_data + instance_num;
        rsp->instance_info.dc.accelid = instance_data->accelid;
        rsp->instance_info.dc.bank_number = instance_data->bank_number;
        rsp->instance_info.dc.is_polled = instance_data->is_polled;
        rsp->instance_info.dc.core_affinity = instance_data->core_affinity;
        rsp->instance_info.dc.num_concurrent_requests =
            instance_data->num_concurrent_requests;
        rsp->instance_info.dc.ring_tx = instance_data->ring_tx;
        rsp->instance_info.dc.ring_rx = instance_data->ring_rx;

        build_msg_header(
            rsp, QATMGR_MSGTYPE_INSTANCE_INFO, sizeof(rsp->instance_info));
    }
    else if (instance_type == SERV_TYPE_CY)
    {
        if (instance_num >= device->num_cy_inst)
        {
            qat_log(LOG_LEVEL_ERROR,
                    "Bad cy instance number %d for section %d\n",
                    instance_num,
                    index);
            err_msg(rsp, "Invalid CY instance number");
            return -1;
        }
        cy_instance_data = device->cy_instance_data + instance_num;
        instance_data = &cy_instance_data->sym;
        /* for CyxIsPooled CY only and SYM only this param is taken from sym
         * instance but for ASMy only from asym */
        if (device->services == SERV_TYPE_ASYM ||
            device->services == SERV_TYPE_ASYM_DC)
        {
            rsp->instance_info.cy.is_polled = cy_instance_data->asym.is_polled;
        }
        else
        {
            rsp->instance_info.cy.is_polled = cy_instance_data->sym.is_polled;
        }
        rsp->instance_info.cy.sym.accelid = instance_data->accelid;
        rsp->instance_info.cy.sym.bank_number = instance_data->bank_number;
        rsp->instance_info.cy.sym.is_polled = instance_data->is_polled;
        rsp->instance_info.cy.sym.core_affinity = instance_data->core_affinity;
        rsp->instance_info.cy.sym.num_concurrent_requests =
            instance_data->num_concurrent_requests;
        rsp->instance_info.cy.sym.ring_tx = instance_data->ring_tx;
        rsp->instance_info.cy.sym.ring_rx = instance_data->ring_rx;

        instance_data = &cy_instance_data->asym;
        rsp->instance_info.cy.asym.accelid = instance_data->accelid;
        rsp->instance_info.cy.asym.bank_number = instance_data->bank_number;
        rsp->instance_info.cy.asym.is_polled = instance_data->is_polled;
        rsp->instance_info.cy.asym.core_affinity = instance_data->core_affinity;
        rsp->instance_info.cy.asym.num_concurrent_requests =
            instance_data->num_concurrent_requests;
        rsp->instance_info.cy.asym.ring_tx = instance_data->ring_tx;
        rsp->instance_info.cy.asym.ring_rx = instance_data->ring_rx;
        build_msg_header(
            rsp, QATMGR_MSGTYPE_INSTANCE_INFO, sizeof(rsp->instance_info));
    }
    else if (instance_type == SERV_TYPE_SYM)
    {
        if (instance_num >= device->num_sym_inst)
        {
            qat_log(LOG_LEVEL_ERROR,
                    "Bad cy.sym instance number %d for section %d\n",
                    instance_num,
                    index);
            err_msg(rsp, "Invalid SYM instance number");
            return -1;
        }
        cy_instance_data = device->cy_instance_data + instance_num;
        instance_data = &cy_instance_data->sym;
        rsp->instance_info.cy.sym.accelid = instance_data->accelid;
        rsp->instance_info.cy.sym.bank_number = instance_data->bank_number;
        rsp->instance_info.cy.sym.is_polled = instance_data->is_polled;
        rsp->instance_info.cy.sym.core_affinity = instance_data->core_affinity;
        rsp->instance_info.cy.sym.num_concurrent_requests =
            instance_data->num_concurrent_requests;
        rsp->instance_info.cy.sym.ring_tx = instance_data->ring_tx;
        rsp->instance_info.cy.sym.ring_rx = instance_data->ring_rx;
        build_msg_header(
            rsp, QATMGR_MSGTYPE_INSTANCE_INFO, sizeof(rsp->instance_info));
    }
    else if (instance_type == SERV_TYPE_ASYM)
    {
        if (instance_num >= device->num_asym_inst)
        {
            qat_log(LOG_LEVEL_ERROR,
                    "Bad cy.asym instance number %d for section %d\n",
                    instance_num,
                    index);
            err_msg(rsp, "Invalid ASYM instance number");
            return -1;
        }
        cy_instance_data = device->cy_instance_data + instance_num;
        instance_data = &cy_instance_data->asym;
        rsp->instance_info.cy.asym.accelid = instance_data->accelid;
        rsp->instance_info.cy.asym.bank_number = instance_data->bank_number;
        rsp->instance_info.cy.asym.is_polled = instance_data->is_polled;
        rsp->instance_info.cy.asym.core_affinity = instance_data->core_affinity;
        rsp->instance_info.cy.asym.num_concurrent_requests =
            instance_data->num_concurrent_requests;
        rsp->instance_info.cy.asym.ring_tx = instance_data->ring_tx;
        rsp->instance_info.cy.asym.ring_rx = instance_data->ring_rx;
        build_msg_header(
            rsp, QATMGR_MSGTYPE_INSTANCE_INFO, sizeof(rsp->instance_info));
    }
    else
    {
        qat_log(
            LOG_LEVEL_ERROR, "Unsupported instance type %d\n", instance_type);
        err_msg(rsp, "Unknown instance type");
        return -1;
    }

    dump_message(rsp, "Response");
    return 0;
}

int release_section(int index, pthread_t tid, char *name, size_t name_len)
{
    ICP_CHECK_FOR_NULL_PARAM(name);

    if (index < 0 || index >= num_section_data)
    {
        qat_log(LOG_LEVEL_ERROR,
                "Invalid section index %d for thread %lu, section %s\n",
                index,
                tid,
                name);
        return -1;
    }
    if (name_len !=
            ICP_ARRAY_STRLEN_SANITIZE(section_data[index].section_name) ||
        strncmp(name, section_data[index].section_name, name_len))
    {
        qat_log(LOG_LEVEL_ERROR,
                "Incorrect section name %s, expected %s\n",
                name,
                section_data[index].section_name);
        return -1;
    }
    if (section_data[index].assigned_tid != tid)
    {
        qat_log(LOG_LEVEL_ERROR,
                "Incorrect thread %lu for section %s. Expected %lu\n",
                tid,
                name,
                section_data[index].assigned_tid);
        return -1;
    }
    qat_log(LOG_LEVEL_DEBUG, "Released section %s\n", name);
    section_data[index].assigned_tid = 0;
    return 0;
}

static int get_section(pthread_t tid, char **derived_section_name)
{
    int i;
    int assigned = 0;

    if (pthread_mutex_lock(&section_data_mutex))
    {
        qat_log(LOG_LEVEL_ERROR, "Unable to lock section_data mutex\n");
        return -2;
    }

    for (i = 0; i < num_section_data; i++)
    {
        if (section_data[i].assigned_tid)
            continue; /* Assigned to another thread */

        section_data[i].assigned_tid = tid;
        assigned = 1;
        break;
    }

    if (pthread_mutex_unlock(&section_data_mutex))
    {
        qat_log(LOG_LEVEL_ERROR, "Unable to unlock section_data mutex\n");
        return -2;
    }

    if (assigned)
    {
        qat_log(
            LOG_LEVEL_DEBUG, "Got section %s\n", section_data[i].section_name);
        if (derived_section_name)
            *derived_section_name = section_data[i].section_name;
        return i;
    }

    return -1;
}

static int handle_section_request(struct qatmgr_msg_req *req,
                                  struct qatmgr_msg_rsp *rsp,
                                  char **section_name,
                                  pid_t tid,
                                  int *index)
{
    int sec;
    char *derived_name;
    static pid_t pid = 0;
    int name_buf_size;

    ICP_CHECK_FOR_NULL_PARAM(req);
    ICP_CHECK_FOR_NULL_PARAM(rsp);
    ICP_CHECK_FOR_NULL_PARAM(index);

    if (req->hdr.len !=
        sizeof(req->hdr) + ICP_ARRAY_STRLEN_SANITIZE(req->name) + 1)
    {
        qat_log(LOG_LEVEL_ERROR, "Bad length\n");
        err_msg(rsp, "Inconsistent length");
        return -1;
    }

    dump_message(req, "Request");

    if (pid != getpid())
    {
        pid = getpid();
        *index = -1;
        if (*section_name)
            free(*section_name);
        *section_name = NULL;
    }

    if (*section_name != NULL || *index != -1)
    {
        qat_log(LOG_LEVEL_ERROR, "Section already allocated\n");
        err_msg(rsp, "Section already allocated");
        return -1;
    }

    sec = get_section(tid, &derived_name);
    if (sec < 0)
    {
        qat_log(LOG_LEVEL_ERROR, "Couldn't get section %s\n", req->name);
        if (sec == -2)
            err_msg(rsp, "Internal error");
        else
            err_msg(rsp, "No section available");
        return sec;
    }

    *index = sec;
    rsp->hdr.type = QATMGR_MSGTYPE_SECTION_GET;
    rsp->hdr.version = THIS_LIB_VERSION;
    ICP_STRLCPY(rsp->name, derived_name, sizeof(rsp->name));
    rsp->hdr.len = sizeof(rsp->hdr) + ICP_ARRAY_STRLEN_SANITIZE(rsp->name) + 1;

    name_buf_size = ICP_ARRAY_STRLEN_SANITIZE(rsp->name) + 1;
    *section_name = malloc(name_buf_size);
    if (!*section_name)
    {
        qat_log(LOG_LEVEL_ERROR, "Memory allocation failed\n");
        err_msg(rsp, "malloc failed");
        return -1;
    }
    qat_log(LOG_LEVEL_DEBUG,
            "Allocated section %s at %p\n",
            rsp->name,
            *section_name);

    ICP_STRLCPY(*section_name, rsp->name, name_buf_size);
    dump_message(rsp, "Response");
    return 0;
}

static int handle_section_release(struct qatmgr_msg_req *req,
                                  struct qatmgr_msg_rsp *rsp,
                                  char **section_name,
                                  pid_t tid,
                                  int *index)
{
    ICP_CHECK_FOR_NULL_PARAM(req);
    ICP_CHECK_FOR_NULL_PARAM(rsp);
    ICP_CHECK_FOR_NULL_PARAM(index);
    ICP_CHECK_FOR_NULL_PARAM(section_name);

    if (req->hdr.len !=
        sizeof(req->hdr) + ICP_ARRAY_STRLEN_SANITIZE(req->name) + 1)
    {
        qat_log(LOG_LEVEL_ERROR, "Bad length\n");
        err_msg(rsp, "Inconsistent length");
        return -1;
    }

    dump_message(req, "Request");

    if (*section_name == NULL)
    {
        qat_log(LOG_LEVEL_ERROR, "Section not allocated\n");
        err_msg(rsp, "Section not allocated");
        return -1;
    }
    if (release_section(
            *index, tid, req->name, ICP_ARRAY_STRLEN_SANITIZE(req->name)))
    {
        err_msg(rsp, "Failed to release section");
    }
    else
    {
        qat_log(LOG_LEVEL_DEBUG, "Section %s released\n", req->name);
        build_msg_header(rsp, QATMGR_MSGTYPE_SECTION_PUT, 0);
        if (*section_name)
        {
            free(*section_name);
            *section_name = NULL;
            *index = -1;
        }
    }
    dump_message(rsp, "Response");
    return 0;
}

static int handle_get_num_pf_devices(struct qatmgr_msg_req *req,
                                     struct qatmgr_msg_rsp *rsp)
{
    ICP_CHECK_FOR_NULL_PARAM(req);
    ICP_CHECK_FOR_NULL_PARAM(rsp);

    if (req->hdr.len != sizeof(req->hdr))
    {
        qat_log(LOG_LEVEL_ERROR, "Bad length\n");
        err_msg(rsp, "Inconsistent length");
        return -1;
    }

    dump_message(req, "Request");

    if (num_pfs == PF_INFO_UNINITIALISED)
    {
        num_pfs = adf_vfio_init_pfs_info(pf_data,
                                         sizeof(pf_data) / sizeof(pf_data[0]));
    }

    if (num_pfs < 0 || num_pfs > ADF_MAX_PF_DEVICES)
    {
        err_msg(rsp, "Unable to init pfs info");
        qat_log(LOG_LEVEL_ERROR, "Invalid number Pfs\n");
        return -1;
    }

    /* num_pfs will be a positive number and less then ADF_MAX_PF_DEVICES */
    rsp->num_devices = num_pfs;
    build_msg_header(rsp, QATMGR_MSGTYPE_NUM_PF_DEVS, sizeof(rsp->num_devices));

    dump_message(rsp, "Response");
    return 0;
}

static int handle_get_pf_device_info(struct qatmgr_msg_req *req,
                                     struct qatmgr_msg_rsp *rsp)
{
    uint16_t device_num;
    ICP_CHECK_FOR_NULL_PARAM(req);
    ICP_CHECK_FOR_NULL_PARAM(rsp);

    if (req->hdr.len != sizeof(req->hdr) + sizeof(req->device_num))
    {
        qat_log(LOG_LEVEL_ERROR, "Bad length\n");
        err_msg(rsp, "Inconsistent length");
        return -1;
    }

    dump_message(req, "Request");

    if (num_pfs == PF_INFO_UNINITIALISED)
    {
        num_pfs = adf_vfio_init_pfs_info(pf_data,
                                         sizeof(pf_data) / sizeof(pf_data[0]));
    }

    if (num_pfs < 0 || num_pfs > ADF_MAX_PF_DEVICES)
    {
        err_msg(rsp, "Unable to init pfs info");
        qat_log(LOG_LEVEL_ERROR, "Invalid number Pfs\n");
        return -1;
    }

    device_num = req->device_num;

    if (device_num >= num_pfs)
    {
        qat_log(LOG_LEVEL_ERROR,
                "Invalid device number %d from %d devices\n",
                device_num,
                num_pfs);
        err_msg(rsp, "Invalid device number");
        return -1;
    }

    memcpy(&rsp->pf_info, &pf_data[device_num], sizeof(rsp->pf_info));
    build_msg_header(rsp, QATMGR_MSGTYPE_PF_DEV_INFO, sizeof(rsp->pf_info));

    dump_message(rsp, "Response");
    return 0;
}

int handle_message(struct qatmgr_msg_req *req,
                   struct qatmgr_msg_rsp *rsp,
                   char **section_name,
                   pid_t tid,
                   int *index)
{
    ICP_CHECK_FOR_NULL_PARAM(req);
    ICP_CHECK_FOR_NULL_PARAM(rsp);
    ICP_CHECK_FOR_NULL_PARAM(index);
    ICP_CHECK_FOR_NULL_PARAM(section_name);

    if (req->hdr.version != THIS_LIB_VERSION)
    {
        char qatlib_ver_str[VER_STR_LEN];
        char qatmgr_ver_str[VER_STR_LEN];
        VER_STR(req->hdr.version, qatlib_ver_str);
        VER_STR(THIS_LIB_VERSION, qatmgr_ver_str);

        qat_log(LOG_LEVEL_ERROR,
                "qatmgr v%s received msg from incompatible qatlib v%s\n",
                qatmgr_ver_str,
                qatlib_ver_str);
        err_msg(rsp, "Incompatible. qatmgr received msg vX from qatlib vY\n");
        return -1;
    }

    switch (req->hdr.type)
    {
        case QATMGR_MSGTYPE_SECTION_GET:
            return handle_section_request(req, rsp, section_name, tid, index);
        case QATMGR_MSGTYPE_SECTION_PUT:
            return handle_section_release(req, rsp, section_name, tid, index);
        case QATMGR_MSGTYPE_NUM_DEVICES:
            return handle_get_num_devices(req, rsp, *index);
        case QATMGR_MSGTYPE_DEVICE_INFO:
            return handle_get_device_info(req, rsp, *index);
        case QATMGR_MSGTYPE_DEVICE_ID:
            return handle_get_device_id(req, rsp, *index);
        case QATMGR_MSGTYPE_INSTANCE_INFO:
            return handle_get_instance_info(req, rsp, *index);
        case QATMGR_MSGTYPE_INSTANCE_NAME:
            return handle_get_instance_name(req, rsp, *index);
        case QATMGR_MSGTYPE_VFIO_FILE:
            return handle_get_vfio_name(req, rsp, *index);
        case QATMGR_MSGTYPE_NUM_PF_DEVS:
            return handle_get_num_pf_devices(req, rsp);
        case QATMGR_MSGTYPE_PF_DEV_INFO:
            return handle_get_pf_device_info(req, rsp);
        default:
            err_msg(rsp, "Unknown message");
    }

    return -1;
}
