#include <lux/engine/RHI/GraphicDeviceInfo.hpp>

#ifndef __PLATFORM_UNIX__
#   ifdef _WIN32 
#       define _WIN32_DCOM
#       include <iostream>
#       include <tchar.h>
#       include <comdef.h>
#       include <Wbemidl.h>
#   endif
#endif

namespace lux::rhi
{
#ifndef __PLATFORM_UNIX__
    std::vector<GraphicDeviceInfo> GraphicDeviceInfoLoader::getGraphicDeviceInfo()
	{
        std::vector<GraphicDeviceInfo> ret;
        HRESULT hres;
        hres = CoInitializeEx(0, COINIT_MULTITHREADED);
        if (FAILED(hres)) return ret;
        hres = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
        if (FAILED(hres)) {
            CoUninitialize();
            return ret;
        }
        IWbemLocator* pLoc = NULL;
        hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&pLoc);
        if (FAILED(hres)) {
            CoUninitialize();
            return ret;
        }
        IWbemServices* pSvc = NULL;
        hres = pLoc->ConnectServer(_bstr_t(L"root\\CIMV2"), NULL, NULL, 0, NULL, 0, 0, &pSvc);
        if (FAILED(hres)) {
            pLoc->Release();
            CoUninitialize();
            return ret;
        }
        hres = CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
        if (FAILED(hres)) {
            pSvc->Release();
            pLoc->Release();
            CoUninitialize();
            return ret;
        }
        IEnumWbemClassObject* pEnumerator = NULL;
        hres = pSvc->ExecQuery(bstr_t("WQL"),
            bstr_t("SELECT * FROM Win32_VideoController"),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnumerator);
        if (FAILED(hres)) {
            pSvc->Release();
            pLoc->Release();
            CoUninitialize();
            return ret;
        }
        IWbemClassObject* pclsObj;
        ULONG uReturn = 0;
        while (pEnumerator)
        {
            HRESULT hr = pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn);
            if (0 == uReturn)break;
            VARIANT vtProp;
            hr = pclsObj->Get(L"Caption", 0, &vtProp, 0, 0);
            std::wcout << " OS Name : " << vtProp.pbVal << std::endl;
            GraphicDeviceInfo info;
            info.name = (char*)vtProp.pbVal;
            ret.push_back(info);
            VariantClear(&vtProp);
        }
        pSvc->Release();
        pLoc->Release();
        pEnumerator->Release();
        pclsObj->Release();
        CoUninitialize();
        return ret;
	}
#else
	//
#endif
}