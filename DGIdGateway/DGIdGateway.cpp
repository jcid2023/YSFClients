/*
*   Copyright (C) 2016-2020 by Jonathan Naylor G4KLX
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "YSFReflectors.h"
#include "DGIdGateway.h"
#include "DGIdNetwork.h"
#include "IMRSNetwork.h"
#include "YSFNetwork.h"
#include "FCSNetwork.h"
#include "UDPSocket.h"
#include "StopWatch.h"
#include "Version.h"
#include "YSFFICH.h"
#include "Thread.h"
#include "Timer.h"
#include "Utils.h"
#include "Log.h"

#if defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#else
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <pwd.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
const char* DEFAULT_INI_FILE = "DGIdGateway.ini";
#else
const char* DEFAULT_INI_FILE = "/etc/DGIdGateway.ini";
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <clocale>
#include <cmath>

const unsigned char DT_VD_MODE1      = 0x01U;
const unsigned char DT_VD_MODE2      = 0x02U;
const unsigned char DT_VOICE_FR_MODE = 0x04U;
const unsigned char DT_DATA_FR_MODE  = 0x08U;

int main(int argc, char** argv)
{
	const char* iniFile = DEFAULT_INI_FILE;
	if (argc > 1) {
		for (int currentArg = 1; currentArg < argc; ++currentArg) {
			std::string arg = argv[currentArg];
			if ((arg == "-v") || (arg == "--version")) {
				::fprintf(stdout, "DGIdGateway version %s\n", VERSION);
				return 0;
			} else if (arg.substr(0, 1) == "-") {
				::fprintf(stderr, "Usage: DGIdGateway [-v|--version] [filename]\n");
				return 1;
			} else {
				iniFile = argv[currentArg];
			}
		}
	}

	CDGIdGateway* gateway = new CDGIdGateway(std::string(iniFile));

	int ret = gateway->run();

	delete gateway;

	return ret;
}

CDGIdGateway::CDGIdGateway(const std::string& configFile) :
m_callsign(),
m_suffix(),
m_conf(configFile),
m_writer(NULL),
m_gps(NULL)
{
}

CDGIdGateway::~CDGIdGateway()
{
}

int CDGIdGateway::run()
{
	bool ret = m_conf.read();
	if (!ret) {
		::fprintf(stderr, "DGIdGateway: cannot read the .ini file\n");
		return 1;
	}

	setlocale(LC_ALL, "C");

#if !defined(_WIN32) && !defined(_WIN64)
	bool m_daemon = m_conf.getDaemon();
	if (m_daemon) {
		// Create new process
		pid_t pid = ::fork();
		if (pid == -1) {
			::fprintf(stderr, "Couldn't fork() , exiting\n");
			return -1;
		}
		else if (pid != 0) {
			exit(EXIT_SUCCESS);
		}

		// Create new session and process group
		if (::setsid() == -1) {
			::fprintf(stderr, "Couldn't setsid(), exiting\n");
			return -1;
		}

		// Set the working directory to the root directory
		if (::chdir("/") == -1) {
			::fprintf(stderr, "Couldn't cd /, exiting\n");
			return -1;
		}

		// If we are currently root...
		if (getuid() == 0) {
			struct passwd* user = ::getpwnam("mmdvm");
			if (user == NULL) {
				::fprintf(stderr, "Could not get the mmdvm user, exiting\n");
				return -1;
			}

			uid_t mmdvm_uid = user->pw_uid;
			gid_t mmdvm_gid = user->pw_gid;

			// Set user and group ID's to mmdvm:mmdvm
			if (setgid(mmdvm_gid) != 0) {
				::fprintf(stderr, "Could not set mmdvm GID, exiting\n");
				return -1;
			}

			if (setuid(mmdvm_uid) != 0) {
				::fprintf(stderr, "Could not set mmdvm UID, exiting\n");
				return -1;
			}

			// Double check it worked (AKA Paranoia) 
			if (setuid(0) != -1) {
				::fprintf(stderr, "It's possible to regain root - something is wrong!, exiting\n");
				return -1;
			}
		}
	}
#endif

	ret = ::LogInitialise(m_conf.getLogFilePath(), m_conf.getLogFileRoot(), m_conf.getLogFileLevel(), m_conf.getLogDisplayLevel());
	if (!ret) {
		::fprintf(stderr, "DGIdGateway: unable to open the log file\n");
		return 1;
	}

#if !defined(_WIN32) && !defined(_WIN64)
	if (m_daemon) {
		::close(STDIN_FILENO);
		::close(STDOUT_FILENO);
		::close(STDERR_FILENO);
	}
#endif
	m_callsign = m_conf.getCallsign();
	m_suffix   = m_conf.getSuffix();

	sockaddr_storage rptAddr;
	unsigned int rptAddrLen;
	if (CUDPSocket::lookup(m_conf.getRptAddress(), m_conf.getRptPort(), rptAddr, rptAddrLen) != 0) {
		LogError("Unable to resolve the address of the host");
		return 1;
	}

	bool debug            = m_conf.getDebug();
	std::string myAddress = m_conf.getMyAddress();
	unsigned int myPort   = m_conf.getMyPort();

	CYSFNetwork rptNetwork(myAddress, myPort, "MMDVM", rptAddr, rptAddrLen, m_callsign, debug);
	ret = rptNetwork.open();
	if (!ret) {
		::LogError("Cannot open the repeater network port");
		::LogFinalise();
		return 1;
	}

	std::string fileName = m_conf.getYSFNetHosts();
	CYSFReflectors* reflectors = new CYSFReflectors(fileName);
	reflectors->load();

	CIMRSNetwork* imrs = new CIMRSNetwork;
	ret = imrs->open();
	if (!ret) {
		delete imrs;
		imrs = NULL;
	}

	unsigned int currentDGId = 0U;

	CDGIdNetwork* dgIdNetwork[100U];
	for (unsigned int i = 0U; i < 100U; i++)
		dgIdNetwork[i] = NULL; 

	std::vector<DGIdData*> dgIdData = m_conf.getDGIdData();
	for (std::vector<DGIdData*>::const_iterator it = dgIdData.begin(); it != dgIdData.end(); ++it) {
		unsigned int dgid        = (*it)->m_dgId;
		std::string type         = (*it)->m_type;
		bool statc               = (*it)->m_static;
		unsigned int rfHangTime  = (*it)->m_rfHangTime;
		unsigned int netHangTime = (*it)->m_netHangTime;
		bool debug               = (*it)->m_debug;
		
		if (type == "FCS") {
			std::string name         = (*it)->m_name;
			unsigned int local       = (*it)->m_local;
			unsigned int txFrequency = m_conf.getTxFrequency();
			unsigned int rxFrequency = m_conf.getRxFrequency();
			std::string locator      = calculateLocator();
			unsigned int id          = m_conf.getId();
			std::string options      = (*it)->m_options;

			dgIdNetwork[dgid] = new CFCSNetwork(name, local, m_callsign, rxFrequency, txFrequency, locator, id, options, debug);
			dgIdNetwork[dgid]->m_modes       = DT_VD_MODE1 | DT_VD_MODE2 | DT_VOICE_FR_MODE | DT_DATA_FR_MODE;
			dgIdNetwork[dgid]->m_static      = statc;
			dgIdNetwork[dgid]->m_rfHangTime  = rfHangTime;
			dgIdNetwork[dgid]->m_netHangTime = netHangTime;
		} else if (type == "YSF") {
			std::string name   = (*it)->m_name;
			unsigned int local = (*it)->m_local;

			CYSFReflector* reflector = reflectors->findByName(name);
			if (reflector != NULL) {
				dgIdNetwork[dgid] = new CYSFNetwork(local, reflector->m_name, reflector->m_addr, reflector->m_addrLen, m_callsign, debug);;
				dgIdNetwork[dgid]->m_modes       = DT_VD_MODE1 | DT_VD_MODE2 | DT_VOICE_FR_MODE | DT_DATA_FR_MODE;
				dgIdNetwork[dgid]->m_static      = statc;
				dgIdNetwork[dgid]->m_rfHangTime  = rfHangTime;
				dgIdNetwork[dgid]->m_netHangTime = netHangTime;
			}
		} else if (type == "IMRS") {
			if (imrs != NULL) {
				std::vector<IMRSDestination*> destinations = (*it)->m_destinations;
				std::vector<IMRSDest*> dests;
				std::string name = (*it)->m_name;

				for (std::vector<IMRSDestination*>::const_iterator it = destinations.begin(); it != destinations.end(); ++it) {
					sockaddr_storage addr;
					unsigned int addrLen;
					if (CUDPSocket::lookup((*it)->m_address, IMRS_PORT, addr, addrLen) == 0) {
						IMRSDest* dest = new IMRSDest;
						dest->m_dgId    = (*it)->m_dgId;
						dest->m_addr    = addr;
						dest->m_addrLen = addrLen;
						dests.push_back(dest);
					} else {
						LogWarning("Unable to resolve the address for %s", (*it)->m_address.c_str());
					}
				}

				imrs->addDGId(dgid, name, dests, debug);

				dgIdNetwork[dgid] = imrs;
				dgIdNetwork[dgid]->m_modes       = DT_VD_MODE1 | DT_VD_MODE2 | DT_VOICE_FR_MODE | DT_DATA_FR_MODE;
				dgIdNetwork[dgid]->m_static      = true;
				dgIdNetwork[dgid]->m_rfHangTime  = rfHangTime;
				dgIdNetwork[dgid]->m_netHangTime = netHangTime;
			}
		} else if (type == "Parrot") {
			unsigned int local = (*it)->m_local;

			sockaddr_storage addr;
			unsigned int     addrLen;
			if (CUDPSocket::lookup((*it)->m_address, (*it)->m_port, addr, addrLen) == 0) {
				dgIdNetwork[dgid] = new CYSFNetwork(local, "PARROT", addr, addrLen, m_callsign, debug);
				dgIdNetwork[dgid]->m_modes       = DT_VD_MODE1 | DT_VD_MODE2 | DT_VOICE_FR_MODE | DT_DATA_FR_MODE;
				dgIdNetwork[dgid]->m_static      = statc;
				dgIdNetwork[dgid]->m_rfHangTime  = rfHangTime;
				dgIdNetwork[dgid]->m_netHangTime = netHangTime;
			} else {
				LogWarning("Unable to resolve the address for the YSF Parrot");
			}
		} else if (type == "YSF2DMR") {
			unsigned int local = (*it)->m_local;

			sockaddr_storage addr;
			unsigned int     addrLen;
			if (CUDPSocket::lookup((*it)->m_address, (*it)->m_port, addr, addrLen) == 0) {
				dgIdNetwork[dgid] = new CYSFNetwork(local, "YSF2DMR", addr, addrLen, m_callsign, debug);
				dgIdNetwork[dgid]->m_modes       = DT_VD_MODE1 | DT_VD_MODE2;
				dgIdNetwork[dgid]->m_static      = statc;
				dgIdNetwork[dgid]->m_rfHangTime  = rfHangTime;
				dgIdNetwork[dgid]->m_netHangTime = netHangTime;
			} else {
				LogWarning("Unable to resolve the address for YSF2DMR");
			}
		} else if (type == "YSF2NXDN") {
			unsigned int local = (*it)->m_local;

			sockaddr_storage addr;
			unsigned int     addrLen;
			if (CUDPSocket::lookup((*it)->m_address, (*it)->m_port, addr, addrLen) == 0) {
				dgIdNetwork[dgid] = new CYSFNetwork(local, "YSF2NXDN", addr, addrLen, m_callsign, debug);
				dgIdNetwork[dgid]->m_modes       = DT_VD_MODE1 | DT_VD_MODE2;
				dgIdNetwork[dgid]->m_static      = statc;
				dgIdNetwork[dgid]->m_rfHangTime  = rfHangTime;
				dgIdNetwork[dgid]->m_netHangTime = netHangTime;
			} else {
				LogWarning("Unable to resolve the address for YSF2NXDN");
			}
		} else if (type == "YSF2P25") {
			unsigned int local = (*it)->m_local;

			sockaddr_storage addr;
			unsigned int     addrLen;
			if (CUDPSocket::lookup((*it)->m_address, (*it)->m_port, addr, addrLen) == 0) {
				dgIdNetwork[dgid] = new CYSFNetwork(local, "YSF2P25", addr, addrLen, m_callsign, debug);
				dgIdNetwork[dgid]->m_modes       = DT_VOICE_FR_MODE;
				dgIdNetwork[dgid]->m_static      = statc;
				dgIdNetwork[dgid]->m_rfHangTime  = rfHangTime;
				dgIdNetwork[dgid]->m_netHangTime = netHangTime;
			} else {
				LogWarning("Unable to resolve the address for YSF2P25");
			}
		}
		
		if (dgIdNetwork[dgid] != NULL && dgIdNetwork[dgid] != imrs) {
			bool ret = dgIdNetwork[dgid]->open();
			if (!ret) {
				delete dgIdNetwork[dgid];
				dgIdNetwork[dgid] = NULL;
			}
			if (dgIdNetwork[dgid] != NULL && dgIdNetwork[dgid]->m_static) {
				dgIdNetwork[dgid]->link();
				dgIdNetwork[dgid]->link();
				dgIdNetwork[dgid]->link();
			}
		}
	}

	createGPS();

	CTimer inactivityTimer(1000U);

	CStopWatch stopWatch;
	stopWatch.start();

	LogMessage("Starting DGIdGateway-%s", VERSION);

	for (;;) {
		unsigned char buffer[200U];
		memset(buffer, 0U, 200U);

		if (rptNetwork.read(0U, buffer) > 0U) {
			if (::memcmp(buffer + 0U, "YSFD", 4U) == 0) {
				CYSFFICH fich;
				bool valid = fich.decode(buffer + 35U);
				if (valid) {
					unsigned char fi   = fich.getFI();
					unsigned char dt   = fich.getDT();
					unsigned char fn   = fich.getFN();
					unsigned char ft   = fich.getFT();
					unsigned char dgId = fich.getDGId();

					if (dgId != 0U && dgId != currentDGId) {
						if (dgIdNetwork[currentDGId] != NULL && !dgIdNetwork[currentDGId]->m_static) {
							dgIdNetwork[currentDGId]->unlink();
							dgIdNetwork[currentDGId]->unlink();
							dgIdNetwork[currentDGId]->unlink();
						}

						if (dgIdNetwork[dgId] != NULL && !dgIdNetwork[dgId]->m_static) {
							dgIdNetwork[dgId]->link();
							dgIdNetwork[dgId]->link();
							dgIdNetwork[dgId]->link();
						}

						std::string desc = dgIdNetwork[dgId]->getDesc(dgId);
						LogDebug("DG-ID set to %u (%s) via RF", dgId, desc.c_str());
						currentDGId = dgId;
					}

					if (m_gps != NULL)
						m_gps->data(buffer + 14U, buffer + 35U, fi, dt, fn, ft);

					if (currentDGId != 0U && dgIdNetwork[currentDGId] != NULL) {
						// Only allow the wanted modes through
						if ((dt == YSF_DT_VD_MODE1      && (dgIdNetwork[currentDGId]->m_modes & DT_VD_MODE1) != 0U) ||
						    (dt == YSF_DT_DATA_FR_MODE  && (dgIdNetwork[currentDGId]->m_modes & DT_DATA_FR_MODE) != 0U) ||
						    (dt == YSF_DT_VD_MODE2      && (dgIdNetwork[currentDGId]->m_modes & DT_VD_MODE2) != 0U) ||
						    (dt == YSF_DT_VOICE_FR_MODE && (dgIdNetwork[currentDGId]->m_modes & DT_VOICE_FR_MODE) != 0U)) {
							// Set the DG-ID to zero for compatibility
							fich.setDGId(0U);
							fich.encode(buffer + 35U);

							dgIdNetwork[currentDGId]->write(currentDGId, buffer);
						}

						inactivityTimer.setTimeout(dgIdNetwork[currentDGId]->m_rfHangTime);
						inactivityTimer.start();
					}
				}

				if ((buffer[34U] & 0x01U) == 0x01U) {
					if (m_gps != NULL)
						m_gps->reset();
				}
			}
		}

		for (unsigned int i = 1U; i < 100U; i++) {
			if (dgIdNetwork[i] != NULL) {
				unsigned int len = dgIdNetwork[i]->read(i, buffer);
				if (len > 0U && (i == currentDGId || currentDGId == 0U)) {
					if (::memcmp(buffer + 0U, "YSFD", 4U) == 0) {
						CYSFFICH fich;
						bool valid = fich.decode(buffer + 35U);
						if (valid) {
							fich.setDGId(i);
							fich.encode(buffer + 35U);

							rptNetwork.write(0U, buffer);

							inactivityTimer.setTimeout(dgIdNetwork[i]->m_netHangTime);
							inactivityTimer.start();

							if (currentDGId == 0U) {
								std::string desc = dgIdNetwork[i]->getDesc(i);
								LogDebug("DG-ID set to %u (%s) via Network", i, desc.c_str());
								currentDGId = i;
							}
						}
					}
				}
			}
		}

		unsigned int ms = stopWatch.elapsed();
		stopWatch.start();

		rptNetwork.clock(ms);

		for (unsigned int i = 1U; i < 100U; i++) {
			if (dgIdNetwork[i] != NULL)
				dgIdNetwork[i]->clock(ms);
		}

		if (m_writer != NULL)
			m_writer->clock(ms);

		inactivityTimer.clock(ms);
		if (inactivityTimer.isRunning() && inactivityTimer.hasExpired()) {
			if (dgIdNetwork[currentDGId] != NULL && !dgIdNetwork[currentDGId]->m_static) {
				dgIdNetwork[currentDGId]->unlink();
				dgIdNetwork[currentDGId]->unlink();
				dgIdNetwork[currentDGId]->unlink();
			}

			LogDebug("DG-ID set to 0 (None) via timeout");

			currentDGId = 0U;
			inactivityTimer.stop();
		}

		if (ms < 5U)
			CThread::sleep(5U);
	}

	rptNetwork.close();

	if (m_gps != NULL) {
		m_writer->close();
		delete m_writer;
		delete m_gps;
	}

	for (unsigned int i = 1U; i < 100U; i++) {
		if (dgIdNetwork[i] != NULL && dgIdNetwork[i] != imrs) {
			dgIdNetwork[i]->unlink();
			dgIdNetwork[i]->unlink();
			dgIdNetwork[i]->unlink();
			dgIdNetwork[i]->close();
			delete dgIdNetwork[i];
		}
	}

	if (imrs != NULL) {
		imrs->close();
		delete imrs;
	}

	::LogFinalise();

	return 0;
}

void CDGIdGateway::createGPS()
{
	if (!m_conf.getAPRSEnabled())
		return;

	std::string address = m_conf.getAPRSAddress();
	unsigned int port   = m_conf.getAPRSPort();
	std::string suffix  = m_conf.getAPRSSuffix();
	bool debug          = m_conf.getDebug();

	m_writer = new CAPRSWriter(m_callsign, m_suffix, address, port, suffix, debug);

	unsigned int txFrequency = m_conf.getTxFrequency();
	unsigned int rxFrequency = m_conf.getRxFrequency();
	std::string desc         = m_conf.getAPRSDescription();

	m_writer->setInfo(txFrequency, rxFrequency, desc);

	bool enabled = m_conf.getGPSDEnabled();
	if (enabled) {
		std::string address = m_conf.getGPSDAddress();
		std::string port    = m_conf.getGPSDPort();

		m_writer->setGPSDLocation(address, port);
	} else {
		float latitude  = m_conf.getLatitude();
		float longitude = m_conf.getLongitude();
		int height      = m_conf.getHeight();

		m_writer->setStaticLocation(latitude, longitude, height);
	}

	bool ret = m_writer->open();
	if (!ret) {
		delete m_writer;
		m_writer = NULL;
		return;
	}

	m_gps = new CGPS(m_writer);
}

std::string CDGIdGateway::calculateLocator()
{
	std::string locator;

	float latitude  = m_conf.getLatitude();
	float longitude = m_conf.getLongitude();

	if (latitude < -90.0F || latitude > 90.0F)
		return "AA00AA";

	if (longitude < -360.0F || longitude > 360.0F)
		return "AA00AA";

	latitude += 90.0F;

	if (longitude > 180.0F)
		longitude -= 360.0F;

	if (longitude < -180.0F)
		longitude += 360.0F;

	longitude += 180.0F;

	float lon = ::floor(longitude / 20.0F);
	float lat = ::floor(latitude  / 10.0F);

	locator += 'A' + (unsigned int)lon;
	locator += 'A' + (unsigned int)lat;

	longitude -= lon * 20.0F;
	latitude  -= lat * 10.0F;

	lon = ::floor(longitude / 2.0F);
	lat = ::floor(latitude  / 1.0F);

	locator += '0' + (unsigned int)lon;
	locator += '0' + (unsigned int)lat;

	longitude -= lon * 2.0F;
	latitude  -= lat * 1.0F;

	lon = ::floor(longitude / (2.0F / 24.0F));
	lat = ::floor(latitude  / (1.0F / 24.0F));

	locator += 'A' + (unsigned int)lon;
	locator += 'A' + (unsigned int)lat;

	return locator;
}

