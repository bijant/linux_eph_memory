#ifndef EPHMFS_UAPI_H
#define EPHMFS_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define EPHMFS_IOCTL_MAGIC 'e'
#define EPHMFS_IOCTL_ATTEMPT_ON _IO(EPHMFS_IOCTL_MAGIC, 0)
#define EPHMFS_IOCTL_ATTEMPT_OFF _IO(EPHMFS_IOCTL_MAGIC, 1)

#endif /* EPHMFS_UAPI_H */
