// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2015 Intel Corporation. All Rights Reserved.

#ifdef RS_USE_WMF_BACKEND

#if (_MSC_FULL_VER < 180031101)
    #error At least Visual Studio 2013 Update 4 is required to compile this backend
#endif

#include "win-helpers.h"

#include "../types.h"

#include <Cfgmgr32.h>
#include <usbioctl.h>
#include <SetupAPI.h>
#include <comdef.h>

#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "setupapi.lib")

#include <initguid.h>
DEFINE_GUID(GUID_DEVINTERFACE_USB_DEVICE, 0xA5DCBF10L, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, \
    0xC0, 0x4F, 0xB9, 0x51, 0xED);
DEFINE_GUID(GUID_DEVINTERFACE_IMAGE, 0x6bdd1fc6L, 0x810f, 0x11d0, 0xbe, 0xc7, 0x08, 0x00, \
    0x2b, 0xe2, 0x09, 0x2f);

namespace rsimpl
{
    namespace uvc
    {
        std::string hr_to_string(HRESULT hr)
        {
            _com_error err(hr);
            std::wstring errorMessage = err.ErrorMessage();
            std::stringstream ss;
            ss << "HResult 0x" << std::hex << hr << ": \"" << std::string(errorMessage.begin(), errorMessage.end()) << "\"";
            return ss.str();
        }

        void check(const char * call, HRESULT hr, bool to_throw)
        {
            if (FAILED(hr))
            {
                if (to_throw) throw std::runtime_error(to_string() << call << "(...) returned " << hr_to_string(hr));
                // TODO: Log 
            }
        }

        std::string win_to_utf(const WCHAR * s)
        {
            auto len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
            if(len == 0) throw std::runtime_error(to_string() << "WideCharToMultiByte(...) returned 0 and GetLastError() is " << GetLastError());
            std::string buffer(len-1, ' ');
            len = WideCharToMultiByte(CP_UTF8, 0, s, -1, &buffer[0], static_cast<int>(buffer.size())+1, nullptr, nullptr);
            if(len == 0) throw std::runtime_error(to_string() << "WideCharToMultiByte(...) returned 0 and GetLastError() is " << GetLastError());
            return buffer;
        }

        std::vector<std::string> tokenize(std::string string, char separator)
        {
            std::vector<std::string> tokens;
            std::string::size_type i1 = 0;
            while(true)
            {
                auto i2 = string.find(separator, i1);
                if(i2 == std::string::npos)
                {
                    tokens.push_back(string.substr(i1));
                    return tokens;
                }
                tokens.push_back(string.substr(i1, i2-i1));
                i1 = i2+1;
            }
        }

        bool parse_usb_path(int & vid, int & pid, int & mi, std::string & unique_id, const std::string & path)
        {
            auto name = path;
            std::transform(begin(name), end(name), begin(name), ::tolower);
            auto tokens = tokenize(name, '#');
            if(tokens.size() < 1 || tokens[0] != R"(\\?\usb)") return false; // Not a USB device
            if(tokens.size() < 3)
            {
                LOG_ERROR("malformed usb device path: " << name);
                return false;
            }

            auto ids = tokenize(tokens[1], '&');
            if(ids[0].size() != 8 || ids[0].substr(0,4) != "vid_" || !(std::istringstream(ids[0].substr(4,4)) >> std::hex >> vid))
            {
                LOG_ERROR("malformed vid string: " << tokens[1]);
                return false;
            }

            if(ids[1].size() != 8 || ids[1].substr(0,4) != "pid_" || !(std::istringstream(ids[1].substr(4,4)) >> std::hex >> pid))
            {
                LOG_ERROR("malformed pid string: " << tokens[1]);
                return false;
            }

            if(ids[2].size() != 5 || ids[2].substr(0,3) != "mi_" || !(std::istringstream(ids[2].substr(3,2)) >> mi))
            {
                LOG_ERROR("malformed mi string: " << tokens[1]);
                return false;
            }

            ids = tokenize(tokens[2], '&');
            if(ids.size() < 2)
            {
                LOG_ERROR("malformed id string: " << tokens[2]);
                return false;
            }
            unique_id = ids[1];
            return true;
        }

        bool parse_usb_path_from_device_id(int & vid, int & pid, int & mi, std::string & unique_id, const std::string & device_id)
        {
            auto name = device_id;
            std::transform(begin(name), end(name), begin(name), ::tolower);
            auto tokens = tokenize(name, '\\');
            if (tokens.size() < 1 || tokens[0] != R"(usb)") return false; // Not a USB device

            auto ids = tokenize(tokens[1], '&');
            if (ids[0].size() != 8 || ids[0].substr(0, 4) != "vid_" || !(std::istringstream(ids[0].substr(4, 4)) >> std::hex >> vid))
            {
                LOG_ERROR("malformed vid string: " << tokens[1]);
                return false;
            }

            if (ids[1].size() != 8 || ids[1].substr(0, 4) != "pid_" || !(std::istringstream(ids[1].substr(4, 4)) >> std::hex >> pid))
            {
                LOG_ERROR("malformed pid string: " << tokens[1]);
                return false;
            }

            if (ids[2].size() != 5 || ids[2].substr(0, 3) != "mi_" || !(std::istringstream(ids[2].substr(3, 2)) >> mi))
            {
                LOG_ERROR("malformed mi string: " << tokens[1]);
                return false;
            }

            ids = tokenize(tokens[2], '&');
            if (ids.size() < 2)
            {
                LOG_ERROR("malformed id string: " << tokens[2]);
                return false;
            }
            unique_id = ids[1];
            return true;
        }

        bool handle_node(const std::wstring & targetKey, HANDLE h, ULONG index)
        {
            USB_NODE_CONNECTION_DRIVERKEY_NAME key;
            key.ConnectionIndex = index;

            if (!DeviceIoControl(h, IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME, &key, sizeof(key), &key, sizeof(key), nullptr, nullptr))
            {
                return false;
            }

            if (key.ActualLength < sizeof(key)) return false;

            auto alloc = std::malloc(key.ActualLength);
            if (!alloc) throw std::bad_alloc();
            auto pKey = std::shared_ptr<USB_NODE_CONNECTION_DRIVERKEY_NAME>(reinterpret_cast<USB_NODE_CONNECTION_DRIVERKEY_NAME *>(alloc), std::free);

            pKey->ConnectionIndex = index;
            if (DeviceIoControl(h, IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME, pKey.get(), key.ActualLength, pKey.get(), key.ActualLength, nullptr, nullptr))
            {
                //std::wcout << pKey->DriverKeyName << std::endl;
                if (targetKey == pKey->DriverKeyName) {
                    return true;
                }
                else return false;
            }

            return false;
        }

        std::wstring get_path(HANDLE h, ULONG index)
        {
            // get name length
            USB_NODE_CONNECTION_NAME name;
            name.ConnectionIndex = index;
            if (!DeviceIoControl(h, IOCTL_USB_GET_NODE_CONNECTION_NAME, &name, sizeof(name), &name, sizeof(name), nullptr, nullptr))
            {
                return std::wstring(L"");
            }

            // alloc space
            if (name.ActualLength < sizeof(name)) return std::wstring(L"");
            auto alloc = std::malloc(name.ActualLength);
            auto pName = std::shared_ptr<USB_NODE_CONNECTION_NAME>(reinterpret_cast<USB_NODE_CONNECTION_NAME *>(alloc), std::free);

            // get name
            pName->ConnectionIndex = index;
            if (DeviceIoControl(h, IOCTL_USB_GET_NODE_CONNECTION_NAME, pName.get(), name.ActualLength, pName.get(), name.ActualLength, nullptr, nullptr))
            {
                return std::wstring(pName->NodeName);
            }

            return std::wstring(L"");
        }

        std::string handle_usb_hub(const std::wstring & targetKey, const std::wstring & path)
        {
            if (path == L"") return "";
            std::wstring fullPath = L"\\\\.\\" + path;

            HANDLE h = CreateFile(fullPath.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) return "";
            auto h_gc = std::shared_ptr<void>(h, CloseHandle);

            USB_NODE_INFORMATION info{};
            if (!DeviceIoControl(h, IOCTL_USB_GET_NODE_INFORMATION, &info, sizeof(info), &info, sizeof(info), nullptr, nullptr))
                return "";

            // for each port on the hub
            for (ULONG i = 1; i <= info.u.HubInformation.HubDescriptor.bNumberOfPorts; ++i)
            {
                // allocate something or other
                char buf[sizeof(USB_NODE_CONNECTION_INFORMATION_EX)] = { 0 };
                PUSB_NODE_CONNECTION_INFORMATION_EX pConInfo = reinterpret_cast<PUSB_NODE_CONNECTION_INFORMATION_EX>(buf);

                // get info about port i
                pConInfo->ConnectionIndex = i;
                if (!DeviceIoControl(h, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, pConInfo, sizeof(buf), pConInfo, sizeof(buf), nullptr, nullptr))
                {
                    continue;
                }

                // check if device is connected
                if (pConInfo->ConnectionStatus != DeviceConnected)
                {
                    continue; // almost assuredly silently. I think this flag gets set for any port without a device
                }

                // if connected, handle correctly, setting the location info if the device is found
                std::string ret = "";
                if (pConInfo->DeviceIsHub) ret = handle_usb_hub(targetKey, get_path(h, i));
                else
                {
                    if (handle_node(targetKey, h, i))
                    {
                        ret = win_to_utf(fullPath.c_str()) + " " + std::to_string(i);
                    }
                }
                if (ret != "") return ret;
            }

            return "";
        }

        std::string get_usb_port_id(int device_vid, int device_pid,
                                    const std::string& device_uid)
        {
            SP_DEVINFO_DATA devInfo = { sizeof(SP_DEVINFO_DATA) };

            // build a device info represent all imaging devices.
            HDEVINFO device_info = SetupDiGetClassDevsEx(static_cast<const GUID *>(&GUID_DEVINTERFACE_IMAGE),
                nullptr, 
                nullptr, 
                DIGCF_PRESENT,
                nullptr,
                nullptr,
                nullptr);
            if (device_info == INVALID_HANDLE_VALUE) throw std::runtime_error("SetupDiGetClassDevs");
            auto di = std::shared_ptr<void>(device_info, SetupDiDestroyDeviceInfoList);

            // enumerate all imaging devices.
            for (int member_index = 0; ; ++member_index)
            {
                SP_DEVICE_INTERFACE_DATA interfaceData = { sizeof(SP_DEVICE_INTERFACE_DATA) };
                unsigned long buf_size = 0;

                if (SetupDiEnumDeviceInfo(device_info, member_index, &devInfo) == FALSE)
                {
                    if (GetLastError() == ERROR_NO_MORE_ITEMS) break; // stop when none left
                    continue; // silently ignore other errors
                }

                // get the device ID of current device.
                if (CM_Get_Device_ID_Size(&buf_size, devInfo.DevInst, 0) != CR_SUCCESS)
                {
                    LOG_ERROR("CM_Get_Device_ID_Size failed");
                    return "";
                }
                
                auto alloc = std::malloc(buf_size * sizeof(WCHAR) + sizeof(WCHAR));
                if (!alloc) throw std::bad_alloc();
                auto pInstID = std::shared_ptr<WCHAR>(reinterpret_cast<WCHAR *>(alloc), std::free);
                if (CM_Get_Device_ID(devInfo.DevInst, pInstID.get(), buf_size * sizeof(WCHAR) + sizeof(WCHAR), 0) != CR_SUCCESS) 
                {
                    LOG_ERROR("CM_Get_Device_ID failed");
                    return "";
                }

                if (pInstID == nullptr) continue;

                // Check if this is our device 
                int usb_vid, usb_pid, usb_mi; std::string usb_unique_id;
                if (!parse_usb_path_from_device_id(usb_vid, usb_pid, usb_mi, usb_unique_id, std::string(win_to_utf(pInstID.get())))) continue;
                if (usb_vid != device_vid || usb_pid != device_pid || /* usb_mi != device->mi || */ usb_unique_id != device_uid) continue;

                // get parent (composite device) instance
                DEVINST instance;
                if (CM_Get_Parent(&instance, devInfo.DevInst, 0) != CR_SUCCESS)
                {
                    LOG_ERROR("CM_Get_Parent failed");
                    return "";
                }

                // get composite device instance id
                if (CM_Get_Device_ID_Size(&buf_size, instance, 0) != CR_SUCCESS)
                {
                    LOG_ERROR("CM_Get_Device_ID_Size failed");
                    return "";
                }
                alloc = std::malloc(buf_size*sizeof(WCHAR) + sizeof(WCHAR));
                if (!alloc) throw std::bad_alloc();
                pInstID = std::shared_ptr<WCHAR>(reinterpret_cast<WCHAR *>(alloc), std::free);
                if (CM_Get_Device_ID(instance, pInstID.get(), buf_size * sizeof(WCHAR) + sizeof(WCHAR), 0) != CR_SUCCESS) {
                    LOG_ERROR("CM_Get_Device_ID failed");
                    return "";
                }

                // upgrade to DEVINFO_DATA for SetupDiGetDeviceRegistryProperty
                device_info = SetupDiGetClassDevs(nullptr, pInstID.get(), nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE | DIGCF_ALLCLASSES);
                if (device_info == INVALID_HANDLE_VALUE) {
                    LOG_ERROR("SetupDiGetClassDevs failed");
                    return "";
                }
                auto di_gc = std::shared_ptr<void>(device_info, SetupDiDestroyDeviceInfoList);

                interfaceData = { sizeof(SP_DEVICE_INTERFACE_DATA) };
                if (SetupDiEnumDeviceInterfaces(device_info, nullptr, &GUID_DEVINTERFACE_USB_DEVICE, 0, &interfaceData) == FALSE)
                {
                    LOG_ERROR("SetupDiEnumDeviceInterfaces failed");
                    return "";
                }

                // get the SP_DEVICE_INTERFACE_DETAIL_DATA object, and also grab the SP_DEVINFO_DATA object for the device
                buf_size = 0;
                SetupDiGetDeviceInterfaceDetail(device_info, &interfaceData, nullptr, 0, &buf_size, nullptr);
                if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
                {
                    LOG_ERROR("SetupDiGetDeviceInterfaceDetail failed");
                    return "";
                }
                alloc = std::malloc(buf_size);
                if (!alloc) throw std::bad_alloc();
                auto detail_data = std::shared_ptr<SP_DEVICE_INTERFACE_DETAIL_DATA>(reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA *>(alloc), std::free);
                detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
                SP_DEVINFO_DATA parent_data = { sizeof(SP_DEVINFO_DATA) };
                if (!SetupDiGetDeviceInterfaceDetail(device_info, &interfaceData, detail_data.get(), buf_size, nullptr, &parent_data))
                {
                    LOG_ERROR("SetupDiGetDeviceInterfaceDetail failed");
                    return "";
                }

                // get driver key for composite device
                buf_size = 0;
                SetupDiGetDeviceRegistryProperty(device_info, &parent_data, SPDRP_DRIVER, nullptr, nullptr, 0, &buf_size);
                if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
                {
                    LOG_ERROR("SetupDiGetDeviceRegistryProperty failed in an unexpected manner");
                    return "";
                }
                alloc = std::malloc(buf_size);
                if (!alloc) throw std::bad_alloc();
                auto driver_key = std::shared_ptr<BYTE>(reinterpret_cast<BYTE*>(alloc), std::free);
                if (!SetupDiGetDeviceRegistryProperty(device_info, &parent_data, SPDRP_DRIVER, nullptr, driver_key.get(), buf_size, nullptr))
                {
                    LOG_ERROR("SetupDiGetDeviceRegistryProperty failed");
                    return "";
                }

                // contains composite device key
                std::wstring targetKey(reinterpret_cast<const wchar_t*>(driver_key.get()));

                // recursively check all hubs, searching for composite device
                std::wstringstream buf;
                for (int i = 0;; i++)
                { 
                    buf << "\\\\.\\HCD" << i;
                    std::wstring hcd = buf.str();

                    // grab handle
                    HANDLE h = CreateFile(hcd.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                    auto h_gc = std::shared_ptr<void>(h, CloseHandle);
                    if (h == INVALID_HANDLE_VALUE)
                    {
                        LOG_ERROR("CreateFile failed");
                        break;
                    }
                    else
                    {
                        USB_ROOT_HUB_NAME name;

                        // get required space
                        if (!DeviceIoControl(h, IOCTL_USB_GET_ROOT_HUB_NAME, nullptr, 0, &name, sizeof(name), nullptr, nullptr)) {
                            LOG_ERROR("DeviceIoControl failed");
                            return ""; // alt: fail silently and hope its on a different root hub
                        }

                        // alloc space
                        alloc = std::malloc(name.ActualLength);
                        if (!alloc) throw std::bad_alloc();
                        auto pName = std::shared_ptr<USB_ROOT_HUB_NAME>(reinterpret_cast<USB_ROOT_HUB_NAME *>(alloc), std::free);

                        // get name
                        if (!DeviceIoControl(h, IOCTL_USB_GET_ROOT_HUB_NAME, nullptr, 0, pName.get(), name.ActualLength, nullptr, nullptr)) {
                            LOG_ERROR("DeviceIoControl failed");
                            return ""; // alt: fail silently and hope its on a different root hub
                        }

                        // return location if device is connected under this root hub
                        std::string ret = handle_usb_hub(targetKey, std::wstring(pName->RootHubName));
                        if (ret != "") return ret;
                    }
                }
            }
            throw std::exception("could not find camera in windows device tree");
        }
    }
}

#endif