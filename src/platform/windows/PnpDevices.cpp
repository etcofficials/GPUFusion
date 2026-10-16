#include "platform/windows/PnpDevices.h"

#include <windows.h>
#include <initguid.h>
#include <devguid.h>
#include <devpkey.h>
#include <setupapi.h>
#include <cfgmgr32.h>

#include <vector>

namespace gf::hw::win {

namespace {

std::vector<BYTE> rawProperty(HDEVINFO set, SP_DEVINFO_DATA& info, const DEVPROPKEY& key, DEVPROPTYPE expected)
{
    DEVPROPTYPE type = 0;
    DWORD size = 0;
    SetupDiGetDevicePropertyW(set, &info, &key, &type, nullptr, 0, &size, 0);
    if (size == 0)
        return {};
    std::vector<BYTE> buffer(size);
    if (!SetupDiGetDevicePropertyW(set, &info, &key, &type, buffer.data(), size, &size, 0) || type != expected)
        return {};
    return buffer;
}

QString stringProperty(HDEVINFO set, SP_DEVINFO_DATA& info, const DEVPROPKEY& key)
{
    const auto raw = rawProperty(set, info, key, DEVPROP_TYPE_STRING);
    if (raw.size() < sizeof(wchar_t))
        return {};
    return QString::fromWCharArray(reinterpret_cast<const wchar_t*>(raw.data())).trimmed();
}

std::optional<quint32> uint32Property(HDEVINFO set, SP_DEVINFO_DATA& info, const DEVPROPKEY& key)
{
    const auto raw = rawProperty(set, info, key, DEVPROP_TYPE_UINT32);
    if (raw.size() != sizeof(quint32))
        return std::nullopt;
    quint32 v = 0;
    memcpy(&v, raw.data(), sizeof(v));
    return v;
}

QDate dateProperty(HDEVINFO set, SP_DEVINFO_DATA& info, const DEVPROPKEY& key)
{
    const auto raw = rawProperty(set, info, key, DEVPROP_TYPE_FILETIME);
    if (raw.size() != sizeof(FILETIME))
        return {};
    FILETIME ft{};
    memcpy(&ft, raw.data(), sizeof(ft));
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&ft, &st)) // driver dates are stored as UTC midnight
        return {};
    return QDate(st.wYear, st.wMonth, st.wDay);
}

} // namespace

PnpEnumeration enumerateDisplayDevices()
{
    PnpEnumeration result;
    // No DIGCF_PRESENT: include devices that are installed but not currently detected.
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, 0);
    if (set == INVALID_HANDLE_VALUE) {
        result.errors << QStringLiteral("Could not list display devices from Device Manager (error %1).").arg(GetLastError());
        return result;
    }

    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); ++i) {
        PnpDevice d;
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        if (SetupDiGetDeviceInstanceIdW(set, &info, id, MAX_DEVICE_ID_LEN, nullptr))
            d.instanceId = QString::fromWCharArray(id);

        ULONG status = 0, problem = 0;
        const CONFIGRET cr = CM_Get_DevNode_Status(&status, &problem, info.DevInst, 0);
        d.present = (cr == CR_SUCCESS);
        if (d.present) {
            d.hasProblem = (status & DN_HAS_PROBLEM) != 0;
            d.problemCode = d.hasProblem ? quint32(problem) : 0;
            d.started = (status & DN_STARTED) != 0;
        }

        d.description = stringProperty(set, info, DEVPKEY_Device_FriendlyName);
        if (d.description.isEmpty())
            d.description = stringProperty(set, info, DEVPKEY_Device_DeviceDesc);
        d.driverVersion = stringProperty(set, info, DEVPKEY_Device_DriverVersion);
        d.driverDate = dateProperty(set, info, DEVPKEY_Device_DriverDate);
        d.driverProvider = stringProperty(set, info, DEVPKEY_Device_DriverProvider);
        d.infName = stringProperty(set, info, DEVPKEY_Device_DriverInfPath);
        d.locationInfo = stringProperty(set, info, DEVPKEY_Device_LocationInfo);

        if (d.instanceId.startsWith(QLatin1StringView("PCI\\"), Qt::CaseInsensitive)) {
            const auto bus = uint32Property(set, info, DEVPKEY_Device_BusNumber);
            const auto address = uint32Property(set, info, DEVPKEY_Device_Address); // (device << 16) | function
            if (bus && address)
                d.location = PciLocation::fromRaw(*bus, *address >> 16, *address & 0xFFFF);
        }
        result.devices.append(d);
    }
    if (GetLastError() != ERROR_NO_MORE_ITEMS)
        result.errors << QStringLiteral("Device Manager enumeration stopped early (error %1).").arg(GetLastError());
    SetupDiDestroyDeviceInfoList(set);
    return result;
}

} // namespace gf::hw::win
