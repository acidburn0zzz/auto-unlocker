#include "servicestoputils.h"

#ifdef _WIN32
#include <process.h>
#include <Tlhelp32.h>
#include <winbase.h>
#endif

void __stdcall ServiceStopper::StopService_s(std::string serviceName)
{
#ifdef _WIN32
	SERVICE_STATUS_PROCESS ssp;
	ULONGLONG dwStartTime = GetTickCount64();
	DWORD dwBytesNeeded;
	ULONGLONG dwTimeout = 10000; // 30-second time-out
	DWORD dwWaitTime;

	// Get a handle to the SCM database. 

	SC_HANDLE schSCManager = OpenSCManager(
		NULL,                    // local computer
		NULL,                    // ServicesActive database 
		SC_MANAGER_ALL_ACCESS);  // full access rights 

	if (NULL == schSCManager)
	{
		throw ServiceStopException("OpenSCManager failed (%d)", GetLastError());
		return;
	}

	// Get a handle to the service.

	SC_HANDLE schService = OpenService(
		schSCManager,         // SCM database 
		serviceName.c_str(),            // name of service 
		SERVICE_STOP |
		SERVICE_QUERY_STATUS |
		SERVICE_ENUMERATE_DEPENDENTS);

	if (schService == NULL)
	{
		throw ServiceStopException("OpenService failed (%d)", GetLastError());
		CloseServiceHandle(schSCManager);
		return;
	}

	// Make sure the service is not already stopped.

	if (!QueryServiceStatusEx(
		schService,
		SC_STATUS_PROCESS_INFO,
		(LPBYTE)&ssp,
		sizeof(SERVICE_STATUS_PROCESS),
		&dwBytesNeeded))
	{
		throw ServiceStopException("QueryServiceStatusEx failed (%d)", GetLastError());
		goto stop_cleanup;
	}

	if (ssp.dwCurrentState == SERVICE_STOPPED)
	{
		throw ServiceStopException("Service is already stopped.");
		goto stop_cleanup;
	}

	// If a stop is pending, wait for it.

	while (ssp.dwCurrentState == SERVICE_STOP_PENDING)
	{
		throw ServiceStopException("Service stop pending...");

		// Do not wait longer than the wait hint. A good interval is 
		// one-tenth of the wait hint but not less than 1 second  
		// and not more than 10 seconds. 

		dwWaitTime = ssp.dwWaitHint / 10;

		if (dwWaitTime < 1000)
			dwWaitTime = 1000;
		else if (dwWaitTime > 10000)
			dwWaitTime = 10000;

		Sleep(dwWaitTime);

		if (!QueryServiceStatusEx(
			schService,
			SC_STATUS_PROCESS_INFO,
			(LPBYTE)&ssp,
			sizeof(SERVICE_STATUS_PROCESS),
			&dwBytesNeeded))
		{
			throw ServiceStopException("QueryServiceStatusEx failed (%d)", GetLastError());
			goto stop_cleanup;
		}

		if (ssp.dwCurrentState == SERVICE_STOPPED)
		{
			throw ServiceStopException("Service stopped successfully.");
			goto stop_cleanup;
		}

		if (GetTickCount64() - dwStartTime > dwTimeout)
		{
			throw ServiceStopException("Service stop timed out.");
			goto stop_cleanup;
		}
	}

	// If the service is running, dependencies must be stopped first.

	StopDependantServices(schService, schSCManager);

	// Send a stop code to the service.

	if (!ControlService(
		schService,
		SERVICE_CONTROL_STOP,
		(LPSERVICE_STATUS)&ssp))
	{
		throw ServiceStopException("ControlService failed (%d)", GetLastError());
		goto stop_cleanup;
	}

	// Wait for the service to stop.

	while (ssp.dwCurrentState != SERVICE_STOPPED)
	{
		Sleep(ssp.dwWaitHint);
		if (!QueryServiceStatusEx(
			schService,
			SC_STATUS_PROCESS_INFO,
			(LPBYTE)&ssp,
			sizeof(SERVICE_STATUS_PROCESS),
			&dwBytesNeeded))
		{
			throw ServiceStopException("QueryServiceStatusEx failed (%d)", GetLastError());
			goto stop_cleanup;
		}

		if (ssp.dwCurrentState == SERVICE_STOPPED)
			break;

		if (GetTickCount64() - dwStartTime > dwTimeout)
		{
			throw ServiceStopException("Wait timed out");
			goto stop_cleanup;
		}
	}

stop_cleanup:
	CloseServiceHandle(schService);
	CloseServiceHandle(schSCManager);
#endif
}

bool __stdcall ServiceStopper::StopDependantServices(SC_HANDLE schService, SC_HANDLE schSCManager)
{
#ifdef _WIN32
	DWORD i;
	DWORD dwBytesNeeded;
	DWORD dwCount;

	LPENUM_SERVICE_STATUS   lpDependencies = NULL;
	ENUM_SERVICE_STATUS     ess;
	SC_HANDLE               hDepService;
	SERVICE_STATUS_PROCESS  ssp;

	ULONGLONG dwStartTime = GetTickCount64();
	ULONGLONG dwTimeout = 10000; // 10-second time-out

	// Pass a zero-length buffer to get the required buffer size.
	if (EnumDependentServices(schService, SERVICE_ACTIVE,
		lpDependencies, 0, &dwBytesNeeded, &dwCount))
	{
		// If the Enum call succeeds, then there are no dependent
		// services, so do nothing.
		return true;
	}
	else
	{
		if (GetLastError() != ERROR_MORE_DATA)
			return false; // Unexpected error

		// Allocate a buffer for the dependencies.
		lpDependencies = (LPENUM_SERVICE_STATUS)HeapAlloc(
			GetProcessHeap(), HEAP_ZERO_MEMORY, dwBytesNeeded);

		if (!lpDependencies)
			return false;

		__try {
			// Enumerate the dependencies.
			if (!EnumDependentServices(schService, SERVICE_ACTIVE,
				lpDependencies, dwBytesNeeded, &dwBytesNeeded,
				&dwCount))
				return false;

			for (i = 0; i < dwCount; i++)
			{
				ess = *(lpDependencies + i);
				// Open the service.
				hDepService = OpenService(schSCManager,
					ess.lpServiceName,
					SERVICE_STOP | SERVICE_QUERY_STATUS);

				if (!hDepService)
					return false;

				__try {
					// Send a stop code.
					if (!ControlService(hDepService,
						SERVICE_CONTROL_STOP,
						(LPSERVICE_STATUS)&ssp))
						return false;

					// Wait for the service to stop.
					while (ssp.dwCurrentState != SERVICE_STOPPED)
					{
						Sleep(ssp.dwWaitHint);
						if (!QueryServiceStatusEx(
							hDepService,
							SC_STATUS_PROCESS_INFO,
							(LPBYTE)&ssp,
							sizeof(SERVICE_STATUS_PROCESS),
							&dwBytesNeeded))
							return false;

						if (ssp.dwCurrentState == SERVICE_STOPPED)
							break;

						if (GetTickCount64() - dwStartTime > dwTimeout)
							return false;
					}
				}
				__finally
				{
					// Always release the service handle.
					CloseServiceHandle(hDepService);
				}
			}
		}
		__finally
		{
			// Always free the enumeration buffer.
			HeapFree(GetProcessHeap(), 0, lpDependencies);
		}
	}
	return true;
#else
	return false;
#endif
}

bool __stdcall ServiceStopper::StartService_s(std::string serviceName)
{
#ifdef _WIN32
	SERVICE_STATUS_PROCESS ssStatus;
	DWORD dwOldCheckPoint;
	ULONGLONG dwStartTickCount;
	DWORD dwWaitTime;
	DWORD dwBytesNeeded;

	// Get a handle to the SCM database. 

	SC_HANDLE schSCManager = OpenSCManager(
		NULL,                    // local computer
		NULL,                    // servicesActive database 
		SC_MANAGER_ALL_ACCESS);  // full access rights 

	if (NULL == schSCManager)
	{
		throw ServiceStopException("OpenSCManager failed (%d)", GetLastError());
		return false;
	}

	// Get a handle to the service.

	SC_HANDLE schService = OpenService(
		schSCManager,         // SCM database 
		serviceName.c_str(),            // name of service 
		SERVICE_ALL_ACCESS);  // full access 

	if (schService == NULL)
	{
		throw ServiceStopException("OpenService failed (%d)", GetLastError());
		CloseServiceHandle(schSCManager);
		return false;
	}

	// Check the status in case the service is not stopped. 

	if (!QueryServiceStatusEx(
		schService,                     // handle to service 
		SC_STATUS_PROCESS_INFO,         // information level
		(LPBYTE)&ssStatus,             // address of structure
		sizeof(SERVICE_STATUS_PROCESS), // size of structure
		&dwBytesNeeded))              // size needed if buffer is too small
	{
		throw ServiceStopException("QueryServiceStatusEx failed (%d)", GetLastError());
		CloseServiceHandle(schService);
		CloseServiceHandle(schSCManager);
		return false;
	}

	// Check if the service is already running. It would be possible 
	// to stop the service here, but for simplicity this example just returns. 

	if (ssStatus.dwCurrentState != SERVICE_STOPPED && ssStatus.dwCurrentState != SERVICE_STOP_PENDING)
	{
		throw ServiceStopException("Cannot start the service because it is already running");
		CloseServiceHandle(schService);
		CloseServiceHandle(schSCManager);
		return false;
	}

	// Save the tick count and initial checkpoint.

	dwStartTickCount = GetTickCount64();
	dwOldCheckPoint = ssStatus.dwCheckPoint;

	// Wait for the service to stop before attempting to start it.

	while (ssStatus.dwCurrentState == SERVICE_STOP_PENDING)
	{
		// Do not wait longer than the wait hint. A good interval is 
		// one-tenth of the wait hint but not less than 1 second  
		// and not more than 10 seconds. 

		dwWaitTime = ssStatus.dwWaitHint / 10;

		if (dwWaitTime < 1000)
			dwWaitTime = 1000;
		else if (dwWaitTime > 10000)
			dwWaitTime = 10000;

		Sleep(dwWaitTime);

		// Check the status until the service is no longer stop pending. 

		if (!QueryServiceStatusEx(
			schService,                     // handle to service 
			SC_STATUS_PROCESS_INFO,         // information level
			(LPBYTE)&ssStatus,             // address of structure
			sizeof(SERVICE_STATUS_PROCESS), // size of structure
			&dwBytesNeeded))              // size needed if buffer is too small
		{
			throw ServiceStopException("QueryServiceStatusEx failed (%d)\n", GetLastError());
			CloseServiceHandle(schService);
			CloseServiceHandle(schSCManager);
			return false;
		}

		if (ssStatus.dwCheckPoint > dwOldCheckPoint)
		{
			// Continue to wait and check.

			dwStartTickCount = GetTickCount64();
			dwOldCheckPoint = ssStatus.dwCheckPoint;
		}
		else
		{
			if (GetTickCount64() - dwStartTickCount > ssStatus.dwWaitHint)
			{
				throw ServiceStopException("Timeout waiting for service to stop\n");
				CloseServiceHandle(schService);
				CloseServiceHandle(schSCManager);
				return false;
			}
		}
	}

	// Attempt to start the service.

	if (!StartService(
		schService,  // handle to service 
		0,           // number of arguments 
		NULL))      // no arguments 
	{
		throw ServiceStopException("StartService failed (%d)\n", GetLastError());
		CloseServiceHandle(schService);
		CloseServiceHandle(schSCManager);
		return false;
	}

	// Check the status until the service is no longer start pending. 

	if (!QueryServiceStatusEx(
		schService,                     // handle to service 
		SC_STATUS_PROCESS_INFO,         // info level
		(LPBYTE)&ssStatus,             // address of structure
		sizeof(SERVICE_STATUS_PROCESS), // size of structure
		&dwBytesNeeded))              // if buffer too small
	{
		throw ServiceStopException("QueryServiceStatusEx failed (%d)\n", GetLastError());
		CloseServiceHandle(schService);
		CloseServiceHandle(schSCManager);
		return false;
	}

	// Save the tick count and initial checkpoint.

	dwStartTickCount = GetTickCount64();
	dwOldCheckPoint = ssStatus.dwCheckPoint;

	while (ssStatus.dwCurrentState == SERVICE_START_PENDING)
	{
		// Do not wait longer than the wait hint. A good interval is 
		// one-tenth the wait hint, but no less than 1 second and no 
		// more than 10 seconds. 

		dwWaitTime = ssStatus.dwWaitHint / 10;

		if (dwWaitTime < 1000)
			dwWaitTime = 1000;
		else if (dwWaitTime > 10000)
			dwWaitTime = 10000;

		Sleep(dwWaitTime);

		// Check the status again. 

		if (!QueryServiceStatusEx(
			schService,             // handle to service 
			SC_STATUS_PROCESS_INFO, // info level
			(LPBYTE)&ssStatus,             // address of structure
			sizeof(SERVICE_STATUS_PROCESS), // size of structure
			&dwBytesNeeded))              // if buffer too small
		{
			throw ServiceStopException("QueryServiceStatusEx failed (%d)\n", GetLastError());
			break;
		}

		if (ssStatus.dwCheckPoint > dwOldCheckPoint)
		{
			// Continue to wait and check.

			dwStartTickCount = GetTickCount64();
			dwOldCheckPoint = ssStatus.dwCheckPoint;
		}
		else
		{
			if (GetTickCount64() - dwStartTickCount > ssStatus.dwWaitHint)
			{
				// No progress made within the wait hint.
				break;
			}
		}
	}

	// Determine whether the service is running.

	CloseServiceHandle(schService);
	CloseServiceHandle(schSCManager);

	return (ssStatus.dwCurrentState == SERVICE_RUNNING);
#else
	return false;
#endif
}

bool ServiceStopper::KillProcess(std::string procName)
{
#ifdef _WIN32
	HANDLE hndl = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	DWORD dwsma = GetLastError();
	DWORD dwExitCode = 0;
	PROCESSENTRY32  procEntry = { 0 };
	procEntry.dwSize = sizeof(PROCESSENTRY32);

	bool found = false;
	Process32First(hndl, &procEntry);
	do
	{
		if (!strcmpi(procEntry.szExeFile, procName.c_str()))
		{
			found = true;
			break;
		}
		//cout<<"Running Process "<<"          "<<procEntry.szExeFile<<endl;
	} while (Process32Next(hndl, &procEntry));

	if (found)
	{
		HANDLE hHandle;
		hHandle = ::OpenProcess(PROCESS_ALL_ACCESS, 0, procEntry.th32ProcessID);

		::GetExitCodeProcess(hHandle, &dwExitCode);
		return ::TerminateProcess(hHandle, dwExitCode);
	}
	else return false;
#endif
	return false;
}
