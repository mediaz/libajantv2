/**
    @file		mac/infoimpl.cpp
	@copyright	Copyright (C) 2009-2017 AJA Video Systems, Inc.  All rights reserved.
    @brief		Implements the AJASystemInfoImpl class on the Mac platform.
**/

#include "ajabase/system/system.h"
#include "ajabase/system/info.h"
#include "ajabase/system/mac/infoimpl.h"

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <mach/mach.h>

#import <Foundation/Foundation.h>
#include <Availability.h>

AJAStatus
aja_sysctl(const char *name, std::string &result)
{
    AJAStatus ret = AJA_STATUS_FAIL;

    static char tmp_buf[4096];
    size_t size = sizeof(tmp_buf);

    // special cases
    if (strcmp(name, "kern.boottime") == 0)
    {
        // timeval variants

        timeval tv;
        size = sizeof(tv);
        if (sysctlbyname(name, (void *)&tv, &size, NULL, 0) == 0)
        {
            time_t t;
            struct tm *tm;
            t = tv.tv_sec;
            tm = localtime(&t);
            strftime(tmp_buf, sizeof(tmp_buf), "%Y-%m-%d %H:%M:%S", tm);

            result = tmp_buf;
            ret = AJA_STATUS_SUCCESS;
        }
    }
    else if (strcmp(name, "hw.memsize") == 0)
    {
        // int64_t variants

        int64_t v;
        size = sizeof(v);
        if (sysctlbyname(name, (void *)&v, &size, NULL, 0) == 0)
        {
            std::ostringstream oss;
            oss << v;
            result = oss.str();
            ret = AJA_STATUS_SUCCESS;
        }
    }
    else if (strcmp(name, "hw.logicalcpu") == 0)
    {
        // int32_t variants

        int32_t v;
        size = sizeof(v);
        if (sysctlbyname(name, (void *)&v, &size, NULL, 0) == 0)
        {
            std::ostringstream oss;
            oss << v;
            result = oss.str();
            ret = AJA_STATUS_SUCCESS;
        }
    }
    else if (strcmp(name, "aja.osversion") == 0)
    {
        // no sysctl for this, so fake it

#if defined(__MAC_10_10) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_10
        NSOperatingSystemVersion v = [[NSProcessInfo processInfo] operatingSystemVersion];
        std::ostringstream oss;
        oss << v.majorVersion << "." << v.minorVersion << "." << v.patchVersion;
        result = oss.str();
#else
        SInt32 majorVersion,minorVersion,pointVersion;
        Gestalt(gestaltSystemVersionMajor, &majorVersion);
        Gestalt(gestaltSystemVersionMinor, &minorVersion);
        Gestalt(gestaltSystemVersionBugFix, &pointVersion);

        std::ostringstream oss;
        oss << majorVersion << "." << minorVersion << "." << pointVersion;
        result = oss.str();
#endif
        ret = AJA_STATUS_SUCCESS;
    }
    else if (sysctlbyname(name, tmp_buf, &size, NULL, 0) == 0)
    {
        // string case

        result = tmp_buf;
        ret = AJA_STATUS_SUCCESS;
    }

    return ret;
}

AJASystemInfoImpl::AJASystemInfoImpl(int units)
{
    mMemoryUnits = units;
}

AJASystemInfoImpl::~AJASystemInfoImpl()
{

}

AJAStatus
AJASystemInfoImpl::Rescan()
{
    AJAStatus ret = AJA_STATUS_FAIL;

    ret = aja_sysctl("hw.model",        mValueMap[int(AJA_SystemInfoTag_System_Model)]);
    ret = aja_sysctl("kern.hostname",   mValueMap[int(AJA_SystemInfoTag_System_Name)]);
    ret = aja_sysctl("kern.boottime",   mValueMap[int(AJA_SystemInfoTag_System_BootTime)]);
    ret = aja_sysctl("hw.targettype",   mValueMap[int(AJA_SystemInfoTag_OS_ProductName)]);
    ret = aja_sysctl("aja.osversion",   mValueMap[int(AJA_SystemInfoTag_OS_Version)]);
    ret = aja_sysctl("kern.osversion",  mValueMap[int(AJA_SystemInfoTag_OS_VersionBuild)]);
    ret = aja_sysctl("kern.version",    mValueMap[int(AJA_SystemInfoTag_OS_KernelVersion)]);
    ret = aja_sysctl("machdep.cpu.brand_string", mValueMap[int(AJA_SystemInfoTag_CPU_Type)]);
    ret = aja_sysctl("hw.logicalcpu",   mValueMap[int(AJA_SystemInfoTag_CPU_NumCores)]);

    // memory is a little special
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_statistics64_data_t vmstat;
    if(KERN_SUCCESS == host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vmstat, &count))
    {
        int64_t memtotalbytes;
        size_t size = sizeof(memtotalbytes);
        if (sysctlbyname("hw.memsize", (void *)&memtotalbytes, &size, NULL, 0) == 0)
        {
            double totalPages = vmstat.wire_count + vmstat.active_count + vmstat.inactive_count + vmstat.free_count;
            double freePercent = (vmstat.free_count) / totalPages;
            double usedPercent = (vmstat.inactive_count + vmstat.wire_count + vmstat.active_count) / totalPages;

            std::string unitsLabel;
            double divisor = 1.0;
            switch(mMemoryUnits)
            {
                default:
                case AJA_SystemInfoMemoryUnit_Bytes:
                    unitsLabel = "B";
                    break;
                case AJA_SystemInfoMemoryUnit_Kilobytes:
                    unitsLabel = "KB";
                    divisor = 1024.0;
                    break;
                case AJA_SystemInfoMemoryUnit_Megabytes:
                    unitsLabel = "MB";
                    divisor = 1048576.0;
                    break;
                case AJA_SystemInfoMemoryUnit_Gigabytes:
                    unitsLabel = "GB";
                    divisor = 1073741824.0;
                    break;
            }

            std::ostringstream t,u,f;
            t << int64_t(memtotalbytes / divisor) << " " << unitsLabel;
            u << int64_t(memtotalbytes * usedPercent / divisor) << " " << unitsLabel;
            f << int64_t(memtotalbytes * freePercent / divisor) << " " << unitsLabel;

            mValueMap[int(AJA_SystemInfoTag_Mem_Total)] = t.str();
            mValueMap[int(AJA_SystemInfoTag_Mem_Used)] = u.str();
            mValueMap[int(AJA_SystemInfoTag_Mem_Free)] = f.str();

            ret = AJA_STATUS_SUCCESS;
        }
    }

    // Paths
    const char* homePath = getenv("HOME");
    if (homePath != NULL)
    {
        mValueMap[int(AJA_SystemInfoTag_Path_PersistenceStoreUser)] = homePath;
        mValueMap[int(AJA_SystemInfoTag_Path_PersistenceStoreUser)].append("/Library/Preferences/");
    }

    mValueMap[int(AJA_SystemInfoTag_Path_PersistenceStoreSystem)] = "/Users/Shared/Library/Preferences/";

    mValueMap[int(AJA_SystemInfoTag_Path_Applications)] = "/Applications/";
    mValueMap[int(AJA_SystemInfoTag_Path_Utilities)] = "/Applications/AJA Utilities/";

    return ret;
}