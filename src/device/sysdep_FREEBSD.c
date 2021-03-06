/*
 * Copyright (C) Tildeslash Ltd. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 *
 * You must obey the GNU Affero General Public License in all respects
 * for all of the code used other than OpenSSL.
 */

/**
 *  System dependent filesystem methods.
 *
 *  @file
 */

#include "config.h"

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif

#if defined HAVE_SYS_UCRED_H
#include <sys/ucred.h>
#endif

#ifdef HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifdef HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>
#endif

#ifdef HAVE_CTYPE_H
#include <ctype.h>
#endif

#ifdef HAVE_KVM_H
#include <kvm.h>
#endif

#ifdef HAVE_PATHS_H
#include <paths.h>
#endif

#ifdef HAVE_DEVSTAT_H
#include <devstat.h>
#endif

#include "monit.h"
#include "device.h"

// libmonit
#include "system/Time.h"
#include "io/File.h"


/* ------------------------------------------------------------- Definitions */


static struct {
        unsigned long long timestamp;
        struct statinfo disk;
} _statistics = {};


/* --------------------------------------- Static constructor and destructor */


static void __attribute__ ((constructor)) _constructor() {
        _statistics.disk.dinfo = CALLOC(1, sizeof(struct devinfo));
}


static void __attribute__ ((destructor)) _destructor() {
        FREE(_statistics.disk.dinfo);
}


/* ----------------------------------------------------------------- Private */


static unsigned long long _bintimeToMilli(struct bintime *time) {
        return time->sec * 1000 + (((unsigned long long)1000 * (uint32_t)(time->frac >> 32)) >> 32);
}


// Parse the device path like /dev/da0p2 or /dev/gpt/myfilesystemlabel into name:instance -> da:0
static bool _parseDevice(const char *path, Device_T device) {
        if (strlen(path) > 5 && Str_startsWith(path, "/dev/")) {
                // Get the disk map
                size_t len = 0;
                if (sysctlbyname("kern.geom.conftxt", NULL, &len, NULL, 0)) {
                        Log_error("system statistics error -- cannot get kern.geom.conftxt size\n");
                        return false;
                }
                char buf[len + 1];
                if (sysctlbyname("kern.geom.conftxt", buf, &len, NULL, 0)) {
                        Log_error("system statistics error -- cannot get kern.geom.conftxt\n");
                        return false;
                }
                buf[len] = 0;
                // Scan the table for matching label/partition
                char disk[PATH_MAX] = {};
                const char *pathname = path + 5; // cut "/dev/" from the path
                for (const char *cursor = buf; cursor; cursor = strchr(cursor, '\n')) {
                        while (*cursor == '\n') {
                                cursor++;
                        }
                        if (*cursor) {
                                int index;
                                char type[64] = {};
                                char name[PATH_MAX] = {};
                                if (sscanf(cursor, "%d %63s %1023s ", &index, type, name) == 3) {
                                        if (Str_isEqual(type, "DISK")) {
                                                snprintf(disk, sizeof(disk), "%s", name);
                                        } else {
                                                if (Str_isEqual(pathname, name)) {
                                                        // Matching label/partition found, parse the disk
                                                        for (size_t i = 0; disk[i]; i++) {
                                                                if (isdigit(*(disk + i))) {
                                                                        strncpy(device->key, disk, i < sizeof(device->key) ? i : sizeof(device->key) - 1);
                                                                        device->instance = Str_parseInt(disk + i);
                                                                        return true;
                                                                }
                                                        }
                                                }
                                        }
                                }
                        }
                }
        }
        Log_error("filesystem statistics error -- cannot parse device '%s'\n", path);
        return false;
}


static bool _getStatistics(unsigned long long now) {
        // Refresh only if the statistics are older then 1 second (handle also backward time jumps)
        if (now > _statistics.timestamp + 1000 || now < _statistics.timestamp - 1000) {
                if (devstat_getdevs(NULL, &(_statistics.disk)) == -1) {
                        Log_error("filesystem statistics error -- devstat_getdevs: %s\n", devstat_errbuf);
                        return false;
                }
                _statistics.timestamp = now;
        }
        return true;
}


static bool _getDummyDiskActivity(__attribute__ ((unused)) void *_inf) {
        return true;
}


static bool _getBlockDiskActivity(void *_inf) {
        Info_T inf = _inf;
        unsigned long long now = Time_milli();
        bool rv = _getStatistics(now);
        if (rv) {
                for (int i = 0; i < _statistics.disk.dinfo->numdevs; i++) {
                        if (_statistics.disk.dinfo->devices[i].unit_number == inf->filesystem->object.instance && IS(_statistics.disk.dinfo->devices[i].device_name, inf->filesystem->object.key)) {
                                unsigned long long now = _statistics.disk.snap_time * 1000;
                                Statistics_update(&(inf->filesystem->time.read), now, _bintimeToMilli(&(_statistics.disk.dinfo->devices[i].duration[DEVSTAT_READ])));
                                Statistics_update(&(inf->filesystem->read.bytes), now, _statistics.disk.dinfo->devices[i].bytes[DEVSTAT_READ]);
                                Statistics_update(&(inf->filesystem->read.operations),  now, _statistics.disk.dinfo->devices[i].operations[DEVSTAT_READ]);
                                Statistics_update(&(inf->filesystem->time.write), now, _bintimeToMilli(&(_statistics.disk.dinfo->devices[i].duration[DEVSTAT_WRITE])));
                                Statistics_update(&(inf->filesystem->write.bytes), now, _statistics.disk.dinfo->devices[i].bytes[DEVSTAT_WRITE]);
                                Statistics_update(&(inf->filesystem->write.operations), now, _statistics.disk.dinfo->devices[i].operations[DEVSTAT_WRITE]);
                                break;
                        }
                }
        }
        return rv;
}


static unsigned long _mibGetValueByNameUlong(const char *name) {
        long long value = 0LL;
        size_t valueLength = sizeof(value);
        if (sysctlbyname(name, &value, &valueLength, NULL, 0)) {
                Log_error("system statistics error -- cannot get %s value: %s\n", name, STRERROR);
                return -1;
        }
        return value;
}


static bool _updateZfsStatistics(Info_T inf) {
        char memberName[PATH_MAX] = {};

        snprintf(memberName, sizeof(memberName), "kstat.zfs.%.256s.dataset.objset-0x%.256s.nread", inf->filesystem->object.key, inf->filesystem->object.module);
        long long nread = _mibGetValueByNameUlong(memberName);

        snprintf(memberName, sizeof(memberName), "kstat.zfs.%.256s.dataset.objset-0x%.256s.reads", inf->filesystem->object.key, inf->filesystem->object.module);
        long long reads = _mibGetValueByNameUlong(memberName);

        snprintf(memberName, sizeof(memberName), "kstat.zfs.%.256s.dataset.objset-0x%.256s.nwritten", inf->filesystem->object.key, inf->filesystem->object.module);
        long long nwritten = _mibGetValueByNameUlong(memberName);

        snprintf(memberName, sizeof(memberName), "kstat.zfs.%.256s.dataset.objset-0x%.256s.writes", inf->filesystem->object.key, inf->filesystem->object.module);
        long long writes = _mibGetValueByNameUlong(memberName);

        if (nread >= 0 && reads >= 0 && nwritten >= 0 && writes >= 0) {
                unsigned long long now = Time_milli();
                Statistics_update(&(inf->filesystem->read.bytes), now, nread);
                Statistics_update(&(inf->filesystem->read.operations), now, reads);
                Statistics_update(&(inf->filesystem->write.bytes), now, nwritten);
                Statistics_update(&(inf->filesystem->write.operations), now, writes);
                return true;
        } else {
                return false;
        }
}


static bool _getZfsObjsetId(Info_T inf) {
        char previousObjsetId[STRLEN] = {};

        // Prepare the zpool statistics path name (kstat.zfs.<zpool>.dataset).
        char mibZfsStatsName[PATH_MAX] = {};
        snprintf(mibZfsStatsName, sizeof(mibZfsStatsName), "kstat.zfs.%.256s.dataset", inf->filesystem->object.key);

        // Translate the kstat.zfs.<zpool>.dataset name to OID
        int    mibZfsStatsQueryOid[CTL_MAXNAME] = {CTL_SYSCTL, CTL_SYSCTL_NAME2OID};
        size_t mibZfsStatsQueryOidLength = 2;
        int    mibZfsStatsRootOid[CTL_MAXNAME] = {};
        size_t mibZfsStatsRootOidLength = sizeof(mibZfsStatsRootOid);
        if (sysctl(mibZfsStatsQueryOid, mibZfsStatsQueryOidLength, mibZfsStatsRootOid, &mibZfsStatsRootOidLength, mibZfsStatsName, strlen(mibZfsStatsName)) == -1) {
                Log_error("system statistics error -- sysctl for %s -> OID failed: %s\n", mibZfsStatsName, STRERROR);
                return false;
        }
        mibZfsStatsRootOidLength /= sizeof(int);

        //
        // Traverse the kstat.zfs.<zpool>.dataset.* OID tree
        //

        // Set the OID query
        mibZfsStatsQueryOid[0] = CTL_SYSCTL;
        mibZfsStatsQueryOid[1] = CTL_SYSCTL_NEXT;
        memcpy(mibZfsStatsQueryOid + 2, mibZfsStatsRootOid, mibZfsStatsRootOidLength * sizeof(int)); // Copy the tree root to the MIB query

        // Set the OID query length: the MIB header objects (CTL_SYSCTL + CTL_SYSCTL_NEXT) + the OID to traverse
        mibZfsStatsQueryOidLength = 2 + mibZfsStatsRootOidLength;

        // Walk tree members
        while (true) {
                int    mibZfsStatsMemberOid[CTL_MAXNAME] = {};
                size_t mibZfsStatsMemberOidLength = sizeof(mibZfsStatsMemberOid);

                // Get next object
                if (sysctl(mibZfsStatsQueryOid, mibZfsStatsQueryOidLength, mibZfsStatsMemberOid, &mibZfsStatsMemberOidLength, 0, 0) == -1) {
                        if (errno != ENOENT)
                                Log_error("system statistics error -- sysctl for next %s object failed: %s\n", mibZfsStatsName, STRERROR);
                        else
                                break; // No more objects under kstat.zfs.<zpool>.dataset.* tree
                }

                // sysctl result: convert OID byte length to object nesting level
                mibZfsStatsMemberOidLength /= sizeof(int);

                // if whole kstat.zfs.<zpool>.dataset.* tree was traversed, sysctl will continue with next object past the tree => quick check the OID level matches expectation
                if (mibZfsStatsMemberOidLength < mibZfsStatsRootOidLength)
                        break; // We left the kstat.zfs.<zpool>.dataset.* tree

                // make sure the returned object is still within the kstat.zfs.<zpool>.dataset tree
                for (size_t i = 0; i < mibZfsStatsRootOidLength; i++)
                        if (mibZfsStatsMemberOid[i] != mibZfsStatsRootOid[i])
                                return false; // We left the kstat.zfs.<zpool>.dataset tree without finding statistics for this dataset

                //
                // Translate the OID to object name
                //

                // Set the name query
                int mibZfsStatsQueryName[CTL_MAXNAME] = {CTL_SYSCTL, CTL_SYSCTL_NAME};
                memcpy(mibZfsStatsQueryName + 2, mibZfsStatsMemberOid, mibZfsStatsMemberOidLength * sizeof(int)); // Copy the member OID the MIB name query
                // Set the name query length: the MIB header objects (CTL_SYSCTL + CTL_SYSCTL_NAME) + the OID to get name for
                size_t mibZfsStatsQueryNameLength = 2 + mibZfsStatsMemberOidLength;

                // translate the OID to name
                char   objsetName[PATH_MAX] = {};
                size_t objsetNameLength = sizeof(objsetName);
                if (sysctl(mibZfsStatsQueryName, mibZfsStatsQueryNameLength, objsetName, &objsetNameLength, 0, 0) == -1 || objsetNameLength <= 0)
                        Log_error("system statistics error -- sysctl for OID -> name failed: %s\n", STRERROR);

                // Process the given objset ID data
                snprintf(mibZfsStatsName, sizeof(mibZfsStatsName), "kstat.zfs.%.256s.dataset.objset-0x", inf->filesystem->object.key);
                if (Str_startsWith(objsetName, mibZfsStatsName)) {
                        // Dissect objset-0x<hex> from the object name (example: kstat.zfs.zroot.dataset.objset-0x4f.nunlinked).
                        char *objsetId = objsetName + strlen(mibZfsStatsName);
                        Str_replaceChar(objsetId, '.', 0);

                        // If we found new objset ID, fetch the dataset_name value and if matching the filesystem we're testing, fetch data and stop, otherwise continue
                        if (! Str_isByteEqual(objsetId, previousObjsetId)) {
                                strncpy(previousObjsetId, objsetId, sizeof(previousObjsetId) - 1);

                                char   memberName[PATH_MAX] = {};
                                char   datasetName[STRLEN];
                                size_t datasetNameLength = sizeof(datasetName);
                                snprintf(memberName, sizeof(memberName), "kstat.zfs.%.256s.dataset.objset-0x%s.dataset_name", inf->filesystem->object.key, objsetId);
                                if (sysctlbyname(memberName, datasetName, &datasetNameLength, NULL, 0)) {
                                        Log_error("system statistics error -- cannot get %s\n", memberName);
                                        return false;
                                }

                                if (Str_isByteEqual(datasetName, inf->filesystem->object.device)) {
                                        // Cache the objset ID, so we can fetch the data directly next time
                                        strncpy(inf->filesystem->object.module, objsetId, sizeof(inf->filesystem->object.module) - 1);
                                        return true;
                                }
                        }
                }
                // Update the query for the next cycle with the current member
                memcpy(mibZfsStatsQueryOid + 2, mibZfsStatsMemberOid, mibZfsStatsMemberOidLength * sizeof(int));
                mibZfsStatsQueryOidLength = 2 + mibZfsStatsMemberOidLength;
        }
        return false;
}


static bool _getZfsDiskActivity(void *_inf) {
        Info_T inf = _inf;

        if (STR_UNDEF(inf->filesystem->object.key)) {
                // Skip if no zpool name is available
                return true;
        }

        // We cache the objset ID in the object.module ... if not set, scan the system information
        if (STR_UNDEF(inf->filesystem->object.module)) {
                if (! _getZfsObjsetId(inf))
                        return false;
        }

        return _updateZfsStatistics(inf);
}


static bool _getDiskUsage(void *_inf) {
        Info_T inf = _inf;
        struct statfs usage;
        if (statfs(inf->filesystem->object.mountpoint, &usage) != 0) {
                Log_error("Error getting usage statistics for filesystem '%s' -- %s\n", inf->filesystem->object.mountpoint, STRERROR);
                return false;
        }
        inf->filesystem->f_bsize = usage.f_bsize;
        inf->filesystem->f_blocks = usage.f_blocks;
        inf->filesystem->f_blocksfree = usage.f_bavail;
        inf->filesystem->f_blocksfreetotal = usage.f_bfree;
        inf->filesystem->f_files = usage.f_files;
        inf->filesystem->f_filesfree = usage.f_ffree;
        return true;
}


static bool _compareMountpoint(const char *mountpoint, struct statfs *mnt) {
        return IS(mountpoint, mnt->f_mntonname);
}


static bool _compareDevice(const char *device, struct statfs *mnt) {
        return IS(device, mnt->f_mntfromname);
}


static void _filesystemFlagsToString(Info_T inf, unsigned long long flags) {
        struct mystable {
                unsigned long long flag;
                char *description;
        } t[]= {
#ifdef MNT_AUTOMOUNTED
                {MNT_AUTOMOUNTED, "automounted"},
#endif
#ifdef MNT_NFS4ACLS
                {MNT_NFS4ACLS, "nfs4acls"},
#endif
#ifdef MNT_SUJ
                {MNT_SUJ, "journaled soft updates"},
#endif
                {MNT_RDONLY, "ro"},
                {MNT_SYNCHRONOUS, "synchronous"},
                {MNT_NOEXEC, "noexec"},
                {MNT_NOSUID, "nosuid"},
                {MNT_UNION, "union"},
                {MNT_ASYNC, "async"},
                {MNT_SUIDDIR, "suiddir"},
                {MNT_SOFTDEP, "soft updates"},
                {MNT_NOSYMFOLLOW, "nosymfollow"},
                {MNT_GJOURNAL, "GEOM journal"},
                {MNT_MULTILABEL, "multilabel"},
                {MNT_ACLS, "acls"},
                {MNT_NOATIME, "noatime"},
                {MNT_NOCLUSTERR, "noclusterr"},
                {MNT_NOCLUSTERW, "noclusterw"},
                {MNT_EXRDONLY, "exported read only"},
                {MNT_EXPORTED, "exported"},
                {MNT_DEFEXPORTED, "exported to the world"},
                {MNT_EXPORTANON, "anon uid mapping"},
                {MNT_EXKERB, "exported with kerberos"},
                {MNT_EXPUBLIC, "public export"},
                {MNT_LOCAL, "local"},
                {MNT_QUOTA, "quota"},
                {MNT_ROOTFS, "rootfs"},
                {MNT_USER, "user"},
                {MNT_IGNORE, "ignore"}
        };
        Util_swapFilesystemFlags(&(inf->filesystem->flags));
        for (size_t i = 0, count = 0; i < sizeof(t) / sizeof(t[0]); i++) {
                if (flags & t[i].flag) {
                        snprintf(inf->filesystem->flags.current + strlen(inf->filesystem->flags.current), sizeof(inf->filesystem->flags.value[0]) - strlen(inf->filesystem->flags.current) - 1, "%s%s", count++ ? ", " : "", t[i].description);
                }
        }
}


static bool _setDevice(Info_T inf, const char *path, bool (*compare)(const char *path, struct statfs *mnt)) {
        int countfs = getfsstat(NULL, 0, MNT_NOWAIT);
        if (countfs != -1) {
                struct statfs *mnt = CALLOC(countfs, sizeof(struct statfs));
                if ((countfs = getfsstat(mnt, countfs * sizeof(struct statfs), MNT_NOWAIT)) != -1) {
                        for (int i = 0; i < countfs; i++) {
                                struct statfs *mntItem = mnt + i;
                                if (compare(path, mntItem)) {
                                        if (IS(mntItem->f_fstypename, "ufs")) {
                                                if (_parseDevice(mntItem->f_mntfromname, &(inf->filesystem->object))) {
                                                        inf->filesystem->object.getDiskActivity = _getBlockDiskActivity;
                                                } else {
                                                        inf->filesystem->object.getDiskActivity = _getDummyDiskActivity;
                                                        DEBUG("I/O monitoring for filesystem '%s' skipped - unable to parse the device %s\n", path, mntItem->f_mntfromname);
                                                }
                                        } else if (IS(mntItem->f_fstypename, "zfs")) {
                                                // ZFS
                                                inf->filesystem->object.getDiskActivity = _getZfsDiskActivity;
                                                // Need base zpool name for kstat.zfs.<NAME> lookup:
                                                snprintf(inf->filesystem->object.key, sizeof(inf->filesystem->object.key), "%s", inf->filesystem->object.device);
                                                Str_replaceChar(inf->filesystem->object.key, '/', 0);
                                        } else {
						inf->filesystem->object.getDiskActivity = _getDummyDiskActivity;
                                        }
                                        inf->filesystem->object.flags = mntItem->f_flags & MNT_VISFLAGMASK;
                                        _filesystemFlagsToString(inf, inf->filesystem->object.flags);
                                        strncpy(inf->filesystem->object.device, mntItem->f_mntfromname, sizeof(inf->filesystem->object.device) - 1);
                                        strncpy(inf->filesystem->object.mountpoint, mntItem->f_mntonname, sizeof(inf->filesystem->object.mountpoint) - 1);
                                        strncpy(inf->filesystem->object.type, mntItem->f_fstypename, sizeof(inf->filesystem->object.type) - 1);
                                        inf->filesystem->object.getDiskUsage = _getDiskUsage;
                                        inf->filesystem->object.mounted = true;
                                        FREE(mnt);
                                        return true;
                                }
                        }
                }
                FREE(mnt);
        }
        Log_error("Lookup for '%s' filesystem failed\n", path);
error:
        inf->filesystem->object.mounted = false;
        return false;
}


static bool _getDevice(Info_T inf, const char *path, bool (*compare)(const char *path, struct statfs *mnt)) {
        if (_setDevice(inf, path, compare)) {
                return (inf->filesystem->object.getDiskUsage(inf) && inf->filesystem->object.getDiskActivity(inf));
        }
        return false;
}


/* ------------------------------------------------------------------ Public */


bool Filesystem_getByMountpoint(Info_T inf, const char *path) {
        ASSERT(inf);
        ASSERT(path);
        return _getDevice(inf, path, _compareMountpoint);
}


bool Filesystem_getByDevice(Info_T inf, const char *path) {
        ASSERT(inf);
        ASSERT(path);
        return _getDevice(inf, path, _compareDevice);
}

