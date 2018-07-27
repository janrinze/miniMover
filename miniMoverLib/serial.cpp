//****Note, windows only!
#ifndef _WIN32
# include <assert.h>
  assert(false);
#endif

#define WIN32_LEAN_AND_MEAN // Exclude rarely-used stuff from Windows headers
#include <SDKDDKVer.h>
#include <Windows.h>
#include <Setupapi.h>
#include <winioctl.h>
#pragma comment(lib, "Setupapi.lib")
#pragma warning(disable:4996)

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <assert.h>

#include "debug.h"
#include "stream.h"
#include "serial.h"

//------------------------

int SerialHelper::m_portCount = 0;
int SerialHelper::m_defaultPortID = -1;
SerialHelper::PortInfo SerialHelper::portInfo[SerialHelper::m_maxPortCount] = {0};

// enumerate all available ports and find there string name as well
// only usb serial ports have a string name, but that is most serial devices these days
// based on CEnumerateSerial http://www.naughter.com/enumser.html
int SerialHelper::queryForPorts(const char *hint)
{
	m_portCount = 0;
	m_defaultPortID = -1;
	bool hintFound = false;

	HDEVINFO hDevInfoSet = SetupDiGetClassDevsA( &GUID_DEVINTERFACE_COMPORT, 
								NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if(hDevInfoSet != INVALID_HANDLE_VALUE)
	{
		SP_DEVINFO_DATA devInfo;
		devInfo.cbSize = sizeof(SP_DEVINFO_DATA);

		const static int maxLen = 512;
		char tstr[maxLen];

		int nIndex = 0;
		while(SetupDiEnumDeviceInfo(hDevInfoSet, nIndex, &devInfo))
		{
			int nPort = -1;
			HKEY key = SetupDiOpenDevRegKey(hDevInfoSet, &devInfo, DICS_FLAG_GLOBAL, 
											0, DIREG_DEV, KEY_QUERY_VALUE);
			if(key != INVALID_HANDLE_VALUE)
			{
				tstr[0] = '\0';
				ULONG tLen = maxLen;
				DWORD dwType = 0;
				LONG status = RegQueryValueExA(key, "PortName", nullptr, &dwType, reinterpret_cast<LPBYTE>(tstr), &tLen);
				if(ERROR_SUCCESS == status && tLen > 0 && tLen < maxLen)
				{
					// it may be possible for string to be unterminated
					// add an extra terminator just to be safe
					tstr[tLen] = '\0';

					// check for COMxx wher xx is a number
					if (strlen(tstr) > 3 && strncmp(tstr, "COM", 3) == 0 && isdigit(tstr[3]))
					{
						// srip off the number
						nPort = atoi(tstr+3);
						if (nPort != -1)
						{
							// if this triggers we need to increase m_maxPortCount
							assert(m_portCount < m_maxPortCount);
							if(m_portCount < m_maxPortCount)
							{
								sprintf(portInfo[m_portCount].deviceName, "\\\\.\\COM%d", nPort);

								// pick highest port by default, unless already found by hint
								if(!hintFound)
								{
									m_defaultPortID = m_portCount;
								}

								DWORD dwType = 0;
								DWORD dwSize = SERIAL_MAX_DEV_NAME_LEN;
								portInfo[m_portCount].displayName[0] = '\0';
								if(SetupDiGetDeviceRegistryPropertyA(hDevInfoSet, &devInfo, SPDRP_DEVICEDESC, &dwType, (PBYTE)portInfo[m_portCount].displayName, SERIAL_MAX_DEV_NAME_LEN, &dwSize))
								{
									// append device name to display name
									int len = strlen(portInfo[m_portCount].displayName);
									sprintf(portInfo[m_portCount].displayName + len, " (%s)", portInfo[m_portCount].deviceName);

									// check if this port matches our hint
									// for now we take the last match
									if(hint && strstr(portInfo[m_portCount].displayName, hint))
									{
										hintFound = true;
										m_defaultPortID = m_portCount;
									}
								}
								else
								{
									debugPrint(DBG_REPORT, "SerialHelper::queryForPorts() failed to find display name for port %s", portInfo[m_portCount].deviceName);
									sprintf(portInfo[m_portCount].displayName, portInfo[m_portCount].deviceName);
								}

								m_portCount++;
							}
							else
								debugPrint(DBG_WARN, "SerialHelper::queryForPorts() m_maxPortCount exeeded %d", m_maxPortCount);
						}
					}
				}
				else
					debugPrint(DBG_WARN, "SerialHelper::queryForPorts() RegQueryValueEx failed with error %d", status);

				RegCloseKey(key);
			}
			else
				debugPrint(DBG_WARN, "SerialHelper::queryForPorts() SetupDiOpenDevRegKey failed with error %d", GetLastError());

			++nIndex;
		}


		if(m_portCount > 0)
		{
			for(int i=0; i<m_portCount; i++)
			{
				debugPrint(DBG_LOG, "SerialHelper::queryForPorts() found port  %d:%s", i, SerialHelper::getPortDisplayName(i));
			}

			if(m_defaultPortID >= 0)
			{
				if(SerialHelper::getPortDisplayName(m_defaultPortID))
					debugPrint(DBG_LOG, "SerialHelper::queryForPorts() default port: %d:%s", m_defaultPortID, SerialHelper::getPortDisplayName(m_defaultPortID));
				else
					debugPrint(DBG_LOG, "SerialHelper::queryForPorts() default port: %d:NULL", m_defaultPortID);
			}
		}
		else
			debugPrint(DBG_REPORT, "SerialHelper::queryForPorts() failed to find any ports");

		SetupDiDestroyDeviceInfoList(hDevInfoSet);
	}
	else
		debugPrint(DBG_WARN, "SerialHelper::queryForPorts() SetupDiGetClassDevs failed with error %d", GetLastError());
	
	return m_defaultPortID;
}

//------------------------

Serial::Serial() 
	: m_handle(INVALID_HANDLE_VALUE) 
	, m_baudRate(-1)
{
	m_deviceName[0] = '\0';
}

Serial::~Serial() 
{ 
	closeStream(); 
}

/*
bool setupConfig(const char *portStr, HANDLE h)
{
 	COMMCONFIG commConfig = {0};
	DWORD dwSize = sizeof(commConfig);
	commConfig.dwSize = dwSize;
	if(GetDefaultCommConfigA(portStr, &commConfig, &dwSize))
	{
		// Set the default communication configuration
		if (SetCommConfig(h, &commConfig, dwSize))
		{
			return true;
		}
	}
	return false;
}
*/

bool Serial::openSerial(const char *deviceName, int baudRate)
{
	assert(deviceName);
	assert(baudRate > 0);

	bool blocking = false; // set to true to block till data arives
	int timeout_ms = 50;

	if(deviceName)
	{
		// if already connected just return
		if(0 == strcmp(deviceName, m_deviceName) && 
			m_baudRate == baudRate)
			return true;

		// close out any previous connection
		closeStream();
	
		// open serial port
		m_handle = CreateFileA( deviceName, GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if(isOpen())
		{
			//if(setupConfig(deviceName, m_handle))
			{
				// setup serial port baud rate
				DCB dcb = {0};
				if (GetCommState(m_handle, &dcb))
				{
					dcb.BaudRate=baudRate;
					dcb.ByteSize=8;
					dcb.StopBits=ONESTOPBIT;
					dcb.Parity=NOPARITY;
					//dcb.fParity = 0;

					if(SetCommState(m_handle, &dcb))
					{
						COMMTIMEOUTS cto;
						if(GetCommTimeouts(m_handle, &cto))
						{
							cto.ReadIntervalTimeout = (blocking) ? 0 : MAXDWORD; // wait forever or wait ReadTotalTimeoutConstant
							cto.ReadTotalTimeoutConstant = timeout_ms;
							cto.ReadTotalTimeoutMultiplier = 0;
							//cto.WriteTotalTimeoutMultiplier = 20000L / baudRate;
							//cto.WriteTotalTimeoutConstant = 0;

							if(SetCommTimeouts(m_handle, &cto))
							{
								if(SetCommMask(m_handle, EV_BREAK|EV_ERR|EV_RXCHAR))
								{
									if(SetupComm(m_handle, m_max_serial_recieve_buf, m_max_serial_recieve_buf))
									{
										clear();

										strcpy(m_deviceName, deviceName);
										m_baudRate = baudRate;

										debugPrint(DBG_LOG, "Serial::openSerial() connected to %s : %d", m_deviceName, m_baudRate);

										return true;
									}
									else
										debugPrint(DBG_WARN, "Serial::openSerial() SetupComm failed with error %d", GetLastError());
								}
								else
									debugPrint(DBG_WARN, "Serial::openSerial() SetCommMask failed with error %d", GetLastError());
							}
							else
								debugPrint(DBG_WARN, "Serial::openSerial() SetCommTimeouts failed with error %d", GetLastError());
						}
						else
							debugPrint(DBG_WARN, "Serial::openSerial() GetCommTimeouts failed with error %d", GetLastError());
					}
					else
						debugPrint(DBG_WARN, "Serial::openSerial() SetCommState failed with error %d", GetLastError());
				}
				else
					debugPrint(DBG_WARN, "Serial::openSerial() GetCommState failed with error %d", GetLastError());
			}
		}
		else
			debugPrint(DBG_WARN, "Serial::openSerial() CreateFile failed with error %d", GetLastError());
	}
	else
		debugPrint(DBG_WARN, "Serial::openSerial() failed invalid input");

	// Close if already open
	closeStream();

	return false;
}

void Serial::closeStream()
{
	if(isOpen())
	{
		CloseHandle(m_handle);
	}
	m_handle = NULL;
	m_baudRate = -1;
	m_deviceName[0] = '\0';
}

bool Serial::isOpen()
{
	return m_handle != INVALID_HANDLE_VALUE; 
}

void Serial::clear()
{
	// call parent
	Stream::clear();

	if(isOpen())
	{
		// check if we have data waiting, without stalling
		DWORD dwErrorFlags;
		COMSTAT ComStat;
		ClearCommError(m_handle, &dwErrorFlags, &ComStat);
		if(ComStat.cbInQue)
		{
			// log any leftover data
			const int len = 4096;
			char buf[len];
			if(read(buf, len))
				debugPrint(DBG_REPORT, "Serial::clear() leftover data: '%s'", buf);
		}
	}
	else
		debugPrint(DBG_WARN, "Serial::clear() failed invalid connection");
}

int Serial::read(char *buf, int len)
{
	assert(buf);
	assert(len > 0);

	if(buf && len > 0)
	{
		buf[0] = '\0';
		if(isOpen())
		{
#if 0 
			// debug path, dolls out data in small chuncks
			static int tbufBytes = 0;
			static char tbuf[512];
			if(tbufBytes > 0)
			{
				int l = min(34, min(len, tbufBytes));
				strncpy(buf, tbuf, l);
				buf[l] = '\0';

				tbufBytes -= l;
				if(tbufBytes > 0)
					memcpy(tbuf, tbuf+l, tbufBytes);

				debugPrint(DBG_LOG, "Serial::read() cache returned %d bytes", l);

				return l;
			}
			else
			{
				memset(tbuf, 0, sizeof(tbuf));

				DWORD tLen;
				if(ReadFile(m_handle, tbuf, sizeof(tbuf), &tLen, NULL))
				{
					if(tLen > 0)
					{
						tbufBytes = tLen;
						int l = min(34, min(len, tbufBytes));
						strncpy(buf, tbuf, l);
						buf[l] = '\0';

						tbufBytes -= l;
						if(tbufBytes > 0)
							memcpy(tbuf, tbuf+l, tbufBytes);

						debugPrint(DBG_LOG, "Serial::read() returned %d bytes", l);

						return l;
					}
					//else no data yet
				}
				else
					debugPrint(DBG_WARN, "Serial::read() failed with error %d", GetLastError());
			}
#else

			DWORD tLen;
			if(ReadFile(m_handle, buf, len-1, &tLen, NULL))
			{
				if(tLen > 0)
				{
					// success
					buf[tLen] = '\0';
					debugPrint(DBG_LOG, "Serial::read() returned %d bytes", tLen);
					return tLen;
				}
			}
			else
				debugPrint(DBG_WARN, "Serial::read() failed with error %d", GetLastError());
#endif
		}
		else
			debugPrint(DBG_WARN, "Serial::read() failed invalid connection");
	}
	else
		debugPrint(DBG_WARN, "Serial::read() failed invalid input");

	return 0;
}

int Serial::write(const char *buf, int len)
{
	assert(buf);
	assert(len > 0);

	if(buf && len > 0)
	{
		if(isOpen())
		{
			DWORD tLen;
			if(WriteFile(m_handle, buf, len, &tLen, NULL))
			{
				// success
				debugPrint(DBG_LOG, "Serial::write() sent: %d of %d bytes", tLen, len);
				return tLen;
			}
			else
				debugPrint(DBG_ERR, "Serial::write() failed with error %d", GetLastError());
		}
		else
			debugPrint(DBG_WARN, "Serial::write() failed invalid connection");
	}
	else
		debugPrint(DBG_WARN, "Serial::write() failed invalid input");

	return 0;
}
