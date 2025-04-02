#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <dirent.h>

#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <linux/limits.h>

#include "common/list.h"
#include "files.h"
#include "criu-amdgpu.pb-c.h"
#include <xf86drm.h>
#include <libdrm/amdgpu.h>

#include "xmalloc.h"
#include "criu-log.h"
#include "amdgpu_drm.h"
#include "amdgpu_plugin_drm.h"
#include "amdgpu_plugin_util.h"
#include "amdgpu_plugin_topology.h"
#include "amdgpu_plugin_dmabuf.h"

#include "util.h"
#include "common/scm.h"
extern void debug_pause();

/* Return < 0 for error, > 0 for "not a dmabuf" and 0 "is a dmabuf" */
int get_dmabuf_info(int fd, struct stat *st)
{
    char path[PATH_MAX];

    if (read_fd_link(fd, path, sizeof(path)) < 0)
        return -1;

    if (strncmp(path, DMABUF_LINK, strlen(DMABUF_LINK)) != 0) {
        //		ret = record_dumped_fd(fd, false);
        return 1;
    }

    return 0;
}

int amdgpu_plugin_dmabuf_dump(int dmabuf_fd, int id, struct stat *dmabuf_stat) {
    int ret = 0;
    off_t offset = 0;
    char path[PATH_MAX];
    size_t len = 0;
    unsigned char *buf = NULL;

    pr_info("Dumping dmabuf fd = %d\n", dmabuf_fd);
debug_pause();

    CriuDmabufNode *node = xmalloc(sizeof(*node));
    if (!node) {
        pr_err("Failed to allocate memory for dmabuf node\n");
        return -ENOMEM;
    }
    criu_dmabuf_node__init(node);

    /* Populate metadata */
    offset = lseek(dmabuf_fd, 0, SEEK_SET);

    if (offset < 0) {
        pr_err("Failed to seek to the beginning of dmabuf fd\n");
        xfree(node);
        return -EINVAL;
    }
    node->dmabuf_fd = dmabuf_fd;
    node->offset = offset;
    node->dmabuf_fd_flags = fcntl(dmabuf_fd, F_GETFD);
    if (node->dmabuf_fd_flags < 0) {
        pr_err("Failed to get dmabuf fd flags\n");
        xfree(node);
        return -EINVAL;
    }

    // major = major(dmabuf_stat->st_rdev);
    // minor = minor(dmabuf_stat->st_rdev);

    // /* Get gem handle */
    // ret = amdgpu_device_initialize(dmabuf_fd, &major, &minor, &h_dev);
    // if (ret) {
    //     pr_err("Failed to initialize amdgpu device\n");
    //     close(dmabuf_fd);
    //     criu_dmabuf_node__free_unpacked(node, NULL);
    //     xfree(node);
    //     return ret;
    // }

    // node->gem_handle = get_gem_handle(h_dev, dmabuf_fd);
    // if (node->gem_handle < 0) {
    //     pr_err("Failed to get GEM handle for dmabuf dmabuf_fd = %d\n", dmabuf_fd);
    //     xfree(node);
    //     return -EINVAL;
    // }
    node->gem_handle = handle_for_shared_bo_fd(dmabuf_fd);

    if (node->gem_handle < 0) {
        pr_err("Failed to get handle for dmabuf_fd\n");
        xfree(node);
        return -EINVAL;
    }

    // Serialize metadata to a file
    snprintf(path, sizeof(path), IMG_DMABUF_FILE, id);
    len = criu_dmabuf_node__get_packed_size(node);
    buf = xmalloc(len);
    if (!buf) {
        pr_err("Failed to allocate buffer for dmabuf metadata\n");
        xfree(node);
        return -ENOMEM;
    }
    criu_dmabuf_node__pack(node, buf);
    ret = write_img_file(path, buf, len);

    xfree(buf);
    xfree(node);
    return ret;
}

int amdgpu_plugin_dmabuf_restore(int id) {
    char path[PATH_MAX];
	size_t img_size;
	FILE *img_fp = NULL;
    int ret = 0;  // retry needed = false
    CriuDmabufNode *rd = NULL;
	unsigned char *buf = NULL;

    snprintf(path, sizeof(path), IMG_DMABUF_FILE, id);

    // Read serialized metadata
    img_fp = open_img_file(path, false, &img_size);
    if (!img_fp) {
        pr_err("Failed to open dmabuf metadata file: %s\n", path);
        return -EINVAL;
    }

    pr_debug("dmabuf Image file size:%ld\n", img_size);
    buf = xmalloc(img_size);
    if (!buf) {
        pr_perror("Failed to allocate memory");
        return -ENOMEM;
    }

    ret = read_fp(img_fp, buf, img_size);
    if (ret) {
        pr_perror("Unable to read from %s", path);
        xfree(buf);
        return ret;
    }

    rd = criu_dmabuf_node__unpack(NULL, img_size, buf);
    if (rd == NULL) {
        pr_perror("Unable to parse the dmabuf message %d", id);
        xfree(buf);
        fclose(img_fp);
        return -1;
    }
    fclose(img_fp);

    pr_info("dmabuf node dmabuf_fd = %d\n", rd->dmabuf_fd);
    pr_info("dmabuf node gem_handle = %d\n", rd->gem_handle);
    pr_info("dmabuf node offset = %d\n", rd->offset);
    pr_info("dmabuf node dmabuf_fd_flags = %d\n", rd->dmabuf_fd_flags);

    // Match GEM handle with shared_dmabuf list
    int dmabuf_fd = dmabuf_fd_for_handle(rd->gem_handle);
    if (dmabuf_fd == -1) {
        pr_err("Failed to find dmabuf_fd for GEM handle = %d\n",
                                                            rd->gem_handle);
        return true;  // Retry needed
    } else {
        pr_info("Restored dmabuf_fd = %d for GEM handle = %d\n",
                                                dmabuf_fd, rd->gem_handle);
    }

    if (dup2(dmabuf_fd, rd->dmabuf_fd) < 0) {
        pr_err("Failed to duplicate dmabuf_fd %d to %d\n",
                                                    dmabuf_fd, rd->dmabuf_fd);
        ret = -EINVAL;
        goto cleanup;
    }
    ret = dmabuf_fd;

    if (lseek(rd->dmabuf_fd, rd->offset, SEEK_SET) < 0) {
        pr_err("Failed to set offset %d for dmabuf_fd %d\n",
                                                    rd->offset, rd->dmabuf_fd);
        ret = -EINVAL;
        goto cleanup;
    }

    // Set the flags for the duplicated fd
    if (fcntl(rd->dmabuf_fd, F_SETFD, rd->dmabuf_fd_flags) < 0) {
        pr_err("Failed to set flags %d for dmabuf_fd %d\n",
                                            rd->dmabuf_fd_flags, rd->dmabuf_fd);
        ret = -EINVAL;
        goto cleanup;
    }

    pr_info("Successfully duplicated and configured dmabuf_fd %d\n",
                                                                rd->dmabuf_fd);
cleanup:
    
 //   amdgpu_device_deinitialize(h_dev);
    criu_dmabuf_node__free_unpacked(rd, NULL);
    xfree(buf);
    return ret;
}
