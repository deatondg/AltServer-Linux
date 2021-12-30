//
//  AltServerApp.cpp
//  AltServer-Windows
//
//  Created by Riley Testut on 8/30/19.
//  Copyright (c) 2019 Riley Testut. All rights reserved.
//
#include "AltServerApp.h"

#include "AppleAPI.hpp"
#include "ConnectionManager.hpp"
#include "InstallError.hpp"
#include "Signer.hpp"
#include "DeviceManager.hpp"
#include "Archiver.hpp"
#include "ServerError.hpp"

#include "AnisetteDataManager.h"

#include <cpprest/http_client.h>
#include <cpprest/filestream.h>

#include <optional>
#include <filesystem>
#include <regex>

#include <plist/plist.h>

using namespace utility;                    // Common utilities like string conversions
using namespace web;                        // Common features like URIs.
using namespace web::http;                  // Common HTTP functionality
using namespace web::http::client;          // HTTP client features
using namespace concurrency::streams;       // Asynchronous streams

extern std::string temporary_directory();
extern std::string make_uuid();
extern std::vector<unsigned char> readFile(const char* filename);



AltServerApp* AltServerApp::_instance = nullptr;

AltServerApp* AltServerApp::instance()
{
	if (_instance == 0)
	{
		_instance = new AltServerApp();
	}

	return _instance;
}

AltServerApp::AltServerApp() : _appGroupSemaphore(1)
{
}

AltServerApp::~AltServerApp()
{
}

void AltServerApp::Start(HWND windowHandle, HINSTANCE instanceHandle)
{
	ConnectionManager::instance()->Start();
	DeviceManager::instance()->Start();
}

void AltServerApp::Stop()
{
}

pplx::task<std::shared_ptr<Application>> AltServerApp::InstallApplication(std::optional<std::string> filepath, std::shared_ptr<Device> installDevice, std::string appleID, std::string password)
{
	return this->_InstallApplication(filepath, installDevice, appleID, password)
	.then([=](pplx::task<std::shared_ptr<Application>> task) -> pplx::task<std::shared_ptr<Application>> {
		try
		{
			auto application = task.get();
			return pplx::create_task([application]() { 
				return application;
			});
		}
		catch (APIError& error)
		{
			if ((APIErrorCode)error.code() == APIErrorCode::InvalidAnisetteData)
			{
				// Our attempt to re-provision the device as a Mac failed, so reset provisioning and try one more time.
				// This appears to happen when iCloud is running simultaneously, and just happens to provision device at same time as AltServer.
				AnisetteDataManager::instance()->ResetProvisioning();

				this->ShowNotification("Registering PC with Apple...", "This may take a few seconds.");

				// Provisioning device can fail if attempted too soon after previous attempt.
				// As a hack around this, we wait a bit before trying again.
				// 10-11 seconds appears to be too short, so wait for 12 seconds instead.
				sleep(12);

				return this->_InstallApplication(filepath, installDevice, appleID, password);
			}
			else
			{
				throw;
			}
		}
	})
	.then([=](pplx::task<std::shared_ptr<Application>> task) -> std::shared_ptr<Application> {
		try
		{
			auto application = task.get();

			std::stringstream ss;
			ss << application->name() << " was successfully installed on " << installDevice->name() << ".";

			this->ShowNotification("Installation Succeeded", ss.str());

			return application;
		}
		catch (InstallError& error)
		{
			if ((InstallErrorCode)error.code() == InstallErrorCode::Cancelled)
			{
				// Ignore
			}
			else
			{
				this->ShowAlert("Installation Failed", error.localizedDescription());
				throw;
			}
		}
		catch (APIError& error)
		{
			if ((APIErrorCode)error.code() == APIErrorCode::InvalidAnisetteData)
			{
				AnisetteDataManager::instance()->ResetProvisioning();
			}

			this->ShowAlert("Installation Failed", error.localizedDescription());
			throw;
		}
		catch (AnisetteError& error)
		{
			this->ShowAlert("AnisetteData Failed", error.localizedDescription());
			throw;
		}
		catch (Error& error)
		{
			this->ShowAlert("Installation Failed", error.localizedDescription());
			throw;
		}
		catch (std::exception& exception)
		{
			odslog("Exception:" << exception.what());

			this->ShowAlert("Installation Failed", exception.what());
			throw;
		}
	});
}

pplx::task<std::shared_ptr<Application>> AltServerApp::_InstallApplication(std::optional<std::string> filepath, std::shared_ptr<Device> installDevice, std::string appleID, std::string password)
{
    fs::path destinationDirectoryPath(temporary_directory());
    destinationDirectoryPath.append(make_uuid());
    
	auto account = std::make_shared<Account>();
	auto app = std::make_shared<Application>();
	auto team = std::make_shared<Team>();
	auto device = std::make_shared<Device>();
	auto appID = std::make_shared<AppID>();
	auto certificate = std::make_shared<Certificate>();
	auto profile = std::make_shared<ProvisioningProfile>();

	auto session = std::make_shared<AppleAPISession>();

	return pplx::create_task([=]() {
		auto anisetteData = AnisetteDataManager::instance()->FetchAnisetteData();
		return this->Authenticate(appleID, password, anisetteData);
	})
    .then([=](std::pair<std::shared_ptr<Account>, std::shared_ptr<AppleAPISession>> pair)
          {
              *account = *(pair.first);
			  *session = *(pair.second);

			  odslog("Fetching team...");

              return this->FetchTeam(account, session);
          })
    .then([=](std::shared_ptr<Team> tempTeam)
          {
			odslog("Registering device...");

              *team = *tempTeam;
              return this->RegisterDevice(installDevice, team, session);
          })
    .then([=](std::shared_ptr<Device> tempDevice)
          {
			odslog("Fetching certificate...");

              *device = *tempDevice;
              return this->FetchCertificate(team, session);
          })
    .then([=](std::shared_ptr<Certificate> tempCertificate)
          {
              *certificate = *tempCertificate;

			  if (filepath.has_value())
			  {
				  odslog("Importing app...");

				  return pplx::create_task([filepath] {
					  return fs::path(*filepath);
					});
			  }
			  else
			  {
				  odslog("Downloading app...");

				  // Show alert before downloading AltStore.
				  this->ShowInstallationNotification("AltStore", device->name());
				  return this->DownloadApp();
			  }
          })
    .then([=](fs::path downloadedAppPath)
          {
			odslog("Downloaded app!");

              fs::create_directory(destinationDirectoryPath);
              
              auto appBundlePath = UnzipAppBundle(downloadedAppPath.string(), destinationDirectoryPath.string());
			  auto app = std::make_shared<Application>(appBundlePath);

			  if (filepath.has_value())
			  {
				  // Show alert after "downloading" local .ipa.
				  this->ShowInstallationNotification(app->name(), device->name());
			  }
			  else
			  {
				  // Remove downloaded app.

				  try
				  {
					  fs::remove(downloadedAppPath);
				  }
				  catch (std::exception& e)
				  {
					  odslog("Failed to remove downloaded .ipa." << e.what());
				  }
			  }              
              
              return app;
          })
    .then([=](std::shared_ptr<Application> tempApp)
          {
			  odslog("Preparing provisioning profiles!");
              *app = *tempApp;
			  return this->PrepareAllProvisioningProfiles(app, device, team, session);
          })
    .then([=](std::map<std::string, std::shared_ptr<ProvisioningProfile>> profiles)
          {
			  odslog("Installing apps!");
              return this->InstallApp(app, device, team, certificate, profiles);
          })
    .then([=](pplx::task<std::shared_ptr<Application>> task)
          {
			if (fs::exists(destinationDirectoryPath))
			{
				std::string comm = "rm -rf '";
				comm += destinationDirectoryPath.string();
				comm += "'";
				odslog("Removing tmp dir: " << comm);
				system(comm.c_str());
				
				// if (fs::exists(destinationDirectoryPath)) fs::remove_all(destinationDirectoryPath);
			}     

			try
			{
				auto application = task.get();
				return application;
			}
			catch (LocalizedError& error)
			{
				if (error.code() == -22421)
				{
					// Don't know what API call returns this error code, so assume any LocalizedError with -22421 error code
					// means invalid anisette data, then throw the correct APIError.
					throw APIError(APIErrorCode::InvalidAnisetteData);
				}
				else if (error.code() == -29004)
				{
					// Same with -29004, "Environment Mismatch"
					throw APIError(APIErrorCode::InvalidAnisetteData);
				}
				else
				{
					throw;
				}
			}
         });
}

pplx::task<fs::path> AltServerApp::DownloadApp()
{
    fs::path temporaryPath(temporary_directory());
    temporaryPath.append(make_uuid());
    
    auto outputFile = std::make_shared<ostream>();
    
    // Open stream to output file.
    auto task = fstream::open_ostream((temporaryPath.string()))
    .then([=](ostream file)
          {
              *outputFile = file;
              
              uri_builder builder("https://cdn.altstore.io/file/altstore/altstore.ipa");
              
              http_client client(builder.to_uri());
              return client.request(methods::GET);
          })
    .then([=](http_response response)
          {
              printf("Received download response status code:%u\n", response.status_code());
              
              // Write response body into the file.
              return response.body().read_to_end(outputFile->streambuf());
          })
    .then([=](size_t)
          {
              outputFile->close();
              return temporaryPath;
          });
    
    return task;
}

pplx::task<std::pair<std::shared_ptr<Account>, std::shared_ptr<AppleAPISession>>> AltServerApp::Authenticate(std::string appleID, std::string password, std::shared_ptr<AnisetteData> anisetteData)
{
	auto verificationHandler = [=](void)->pplx::task<std::optional<std::string>> {
		return pplx::create_task([=]() -> std::optional<std::string> {
			std::cout << "Enter two factor code" << std::endl;
			std::string _verificationCode = "";
			std::cin >> _verificationCode;
			auto verificationCode = std::make_optional<std::string>(_verificationCode);
			_verificationCode = "";

			return verificationCode;
		});
	};

	return pplx::create_task([=]() {
		if (anisetteData == NULL)
		{
			throw ServerError(ServerErrorCode::InvalidAnisetteData);
		}

		return AppleAPI::getInstance()->Authenticate(appleID, password, anisetteData, verificationHandler);
	});
}

pplx::task<std::shared_ptr<Team>> AltServerApp::FetchTeam(std::shared_ptr<Account> account, std::shared_ptr<AppleAPISession> session)
{
    auto task = AppleAPI::getInstance()->FetchTeams(account, session)
    .then([](std::vector<std::shared_ptr<Team>> teams) {

		for (auto& team : teams)
		{
			if (team->type() == Team::Type::Individual)
			{
				return team;
			}
		}

		for (auto& team : teams)
		{
			if (team->type() == Team::Type::Free)
			{
				return team;
			}
		}

		for (auto& team : teams)
		{
			return team;
		}

		throw InstallError(InstallErrorCode::NoTeam);
    });
    
    return task;
}

pplx::task<std::shared_ptr<Certificate>> AltServerApp::FetchCertificate(std::shared_ptr<Team> team, std::shared_ptr<AppleAPISession> session)
{
    auto task = AppleAPI::getInstance()->FetchCertificates(team, session)
    .then([this, team, session](std::vector<std::shared_ptr<Certificate>> certificates)
          {
			auto certificatesDirectoryPath = this->certificatesDirectoryPath();
			auto cachedCertificatePath = certificatesDirectoryPath.append(team->identifier() + ".p12");

			std::shared_ptr<Certificate> preferredCertificate = nullptr;

			for (auto& certificate : certificates)
			{
				if (!certificate->machineName().has_value())
				{
					continue;
				}

				std::string prefix("AltStore");

				if (certificate->machineName()->size() < prefix.size())
				{
					// Machine name doesn't begin with "AltStore", so ignore.
					continue;
				}
				else
				{
					auto result = std::mismatch(prefix.begin(), prefix.end(), certificate->machineName()->begin());
					if (result.first != prefix.end())
					{
						// Machine name doesn't begin with "AltStore", so ignore.
						continue;
					}
				}

				if (fs::exists(cachedCertificatePath) && certificate->machineIdentifier().has_value())
				{
					try
					{
						auto data = readFile(cachedCertificatePath.string().c_str());
						auto cachedCertificate = std::make_shared<Certificate>(data, *certificate->machineIdentifier());

						// Manually set machineIdentifier so we can encrypt + embed certificate if needed.
						cachedCertificate->setMachineIdentifier(*certificate->machineIdentifier());

						return pplx::create_task([cachedCertificate] {
							return cachedCertificate;
						});
					}
					catch(std::exception &e)
					{
						// Ignore cached certificate errors.
						odslog("Failed to load cached certificate:" << cachedCertificatePath << ". " << e.what())
					}
				}

				preferredCertificate = certificate;

				// Machine name starts with AltStore.

				this->ShowAlert("Installing AltStore with Multiple AltServers Not Supported",
					"Please use the same AltServer you previously used with this Apple ID, or else apps installed with other AltServers will stop working.\n\nAre you sure you want to continue? (Ctrl-C to avoid)");
				break;
			}

              if (certificates.size() != 0)
              {
                  auto certificate = (preferredCertificate != nullptr) ? preferredCertificate : certificates[0];
                  return AppleAPI::getInstance()->RevokeCertificate(certificate, team, session).then([this, team, session](bool success)
                                                                                            {
                                                                                                return this->FetchCertificate(team, session);
                                                                                            });
              }
              else
              {
                  std::string machineName = "AltStore";
                  
                  return AppleAPI::getInstance()->AddCertificate(machineName, team, session)
					  .then([team, session, cachedCertificatePath](std::shared_ptr<Certificate> addedCertificate)
                        {
                            auto privateKey = addedCertificate->privateKey();
                            if (privateKey == std::nullopt)
                            {
                                throw InstallError(InstallErrorCode::MissingPrivateKey);
                            }
                                                                                             
                            return AppleAPI::getInstance()->FetchCertificates(team, session)
                            .then([privateKey, addedCertificate, cachedCertificatePath](std::vector<std::shared_ptr<Certificate>> certificates)
                                {
                                    std::shared_ptr<Certificate> certificate = nullptr;
                                                                                                       
                                    for (auto tempCertificate : certificates)
                                    {
                                        if (tempCertificate->serialNumber() == addedCertificate->serialNumber())
                                        {
                                            certificate = tempCertificate;
                                            break;
                                        }
                                    }
                                                                                                       
                                    if (certificate == nullptr)
                                    {
                                        throw InstallError(InstallErrorCode::MissingCertificate);
                                    }
                                                                                                       
                                    certificate->setPrivateKey(privateKey);

									try
									{
										if (certificate->machineIdentifier().has_value())
										{
											auto machineIdentifier = certificate->machineIdentifier();

											auto encryptedData = certificate->encryptedP12Data(*machineIdentifier);
											if (encryptedData.has_value())
											{
												std::ofstream fout(cachedCertificatePath.string(), std::ios::out | std::ios::binary);
												fout.write((const char*)encryptedData->data(), encryptedData->size());
												fout.close();
											}
										}
									}
									catch (std::exception& e)
									{
										// Ignore caching certificate errors.
										odslog("Failed to cache certificate:" << cachedCertificatePath << ". " << e.what())
									}

                                    return certificate;
                                });
                        });
              }
          });
    
    return task;
}

pplx::task<std::map<std::string, std::shared_ptr<ProvisioningProfile>>> AltServerApp::PrepareAllProvisioningProfiles(
	std::shared_ptr<Application> application,
	std::shared_ptr<Device> device,
	std::shared_ptr<Team> team,
	std::shared_ptr<AppleAPISession> session)
{
	return this->PrepareProvisioningProfile(application, std::nullopt, device, team, session)
	.then([=](std::shared_ptr<ProvisioningProfile> profile) {
		std::vector<pplx::task<std::pair<std::string, std::shared_ptr<ProvisioningProfile>>>> tasks;

		auto task = pplx::create_task([application, profile]() -> std::pair<std::string, std::shared_ptr<ProvisioningProfile>> { 
			return std::make_pair(application->bundleIdentifier(), profile); 
		});
		tasks.push_back(task);

		for (auto appExtension : application->appExtensions())
		{
			auto task = this->PrepareProvisioningProfile(appExtension, application, device, team, session)
			.then([appExtension](std::shared_ptr<ProvisioningProfile> profile) {
				return std::make_pair(appExtension->bundleIdentifier(), profile);
			});
			tasks.push_back(task);
		}

		return pplx::when_all(tasks.begin(), tasks.end())
			.then([tasks](pplx::task<std::vector<std::pair<std::string, std::shared_ptr<ProvisioningProfile>>>> task) {
				try
				{
					auto pairs = task.get();

					std::map<std::string, std::shared_ptr<ProvisioningProfile>> profiles;
					for (auto& pair : pairs)
					{
						profiles[pair.first] = pair.second;
					}

					//observe_all_exceptions<std::pair<std::string, std::shared_ptr<ProvisioningProfile>>>(tasks.begin(), tasks.end());
					return profiles;
				}
				catch (std::exception& e)
				{
					//observe_all_exceptions<std::pair<std::string, std::shared_ptr<ProvisioningProfile>>>(tasks.begin(), tasks.end());
					throw;
				}
		});
	});
}

pplx::task<std::shared_ptr<ProvisioningProfile>> AltServerApp::PrepareProvisioningProfile(
	std::shared_ptr<Application> app,
	std::optional<std::shared_ptr<Application>> parentApp,
	std::shared_ptr<Device> device,
	std::shared_ptr<Team> team,
	std::shared_ptr<AppleAPISession> session)
{
	std::string preferredName;
	std::string parentBundleID;

	if (parentApp.has_value())
	{
		parentBundleID = (*parentApp)->bundleIdentifier();
		preferredName = (*parentApp)->name() + " " + app->name();
	}
	else
	{
		parentBundleID = app->bundleIdentifier();
		preferredName = app->name();
	}

	std::string updatedParentBundleID;

	if (app->isAltStoreApp())
	{
		std::stringstream ss;
		ss << "com." << team->identifier() << "." << parentBundleID;

		updatedParentBundleID = ss.str();
	}
	else
	{
		updatedParentBundleID = parentBundleID + "." + team->identifier();
	}

	std::string bundleID = std::regex_replace(app->bundleIdentifier(), std::regex(parentBundleID), updatedParentBundleID);

	return this->RegisterAppID(preferredName, bundleID, team, session)
	.then([=](std::shared_ptr<AppID> appID)
	{
		return this->UpdateAppIDFeatures(appID, app, team, session);
	})
	.then([=](std::shared_ptr<AppID> appID)
	{
		return this->UpdateAppIDAppGroups(appID, app, team, session);
	})
	.then([=](std::shared_ptr<AppID> appID)
	{
		return this->FetchProvisioningProfile(appID, device, team, session);
	})
	.then([=](std::shared_ptr<ProvisioningProfile> profile)
	{
		return profile;
	});
}

pplx::task<std::shared_ptr<AppID>> AltServerApp::RegisterAppID(std::string appName, std::string bundleID, std::shared_ptr<Team> team, std::shared_ptr<AppleAPISession> session)
{
    auto task = AppleAPI::getInstance()->FetchAppIDs(team, session)
    .then([bundleID, appName, team, session](std::vector<std::shared_ptr<AppID>> appIDs)
          {
              std::shared_ptr<AppID> appID = nullptr;
              
              for (auto tempAppID : appIDs)
              {
                  if (tempAppID->bundleIdentifier() == bundleID)
                  {
                      appID = tempAppID;
                      break;
                  }
              }
              
              if (appID != nullptr)
              {
                  return pplx::task<std::shared_ptr<AppID>>([appID]()
                                                            {
                                                                return appID;
                                                            });
              }
              else
              {
                  return AppleAPI::getInstance()->AddAppID(appName, bundleID, team, session);
              }
          });
    
    return task;
}

pplx::task<std::shared_ptr<AppID>> AltServerApp::UpdateAppIDFeatures(std::shared_ptr<AppID> appID, std::shared_ptr<Application> app, std::shared_ptr<Team> team, std::shared_ptr<AppleAPISession> session)
{
	//TODO: Add support for additional features besides app groups.

	std::map<std::string, plist_t> altstoreFeatures = appID->features(); 

	auto boolNode = plist_new_bool(true);
	altstoreFeatures[AppIDFeatureAppGroups] = boolNode;

	//TODO: Only update features if needed.

	std::shared_ptr<AppID> copiedAppID = std::make_shared<AppID>(*appID);
	copiedAppID->setFeatures(altstoreFeatures);

	plist_free(boolNode);

	return AppleAPI::getInstance()->UpdateAppID(copiedAppID, team, session);
}

pplx::task<std::shared_ptr<AppID>> AltServerApp::UpdateAppIDAppGroups(std::shared_ptr<AppID> appID, std::shared_ptr<Application> app, std::shared_ptr<Team> team, std::shared_ptr<AppleAPISession> session)
{
	return pplx::create_task([=]() -> pplx::task<std::shared_ptr<AppID>> {
		auto applicationGroupsNode = app->entitlements()["com.apple.security.application-groups"];
		std::vector<std::string> applicationGroups;

		if (applicationGroupsNode != nullptr)
		{
			for (int i = 0; i < plist_array_get_size(applicationGroupsNode); i++)
			{
				auto groupNode = plist_array_get_item(applicationGroupsNode, i);

				char* groupName = nullptr;
				plist_get_string_val(groupNode, &groupName);

				applicationGroups.push_back(groupName);
			}
		}

		if (applicationGroups.size() == 0)
		{
			auto appGroupsNode = appID->features()["APG3427HIY"]; // App group feature ID
			if (appGroupsNode != nullptr)
			{
				uint8_t isAppGroupsEnabled = 0;
				plist_get_bool_val(appGroupsNode, &isAppGroupsEnabled);

				if (!isAppGroupsEnabled)
				{
					// No app groups, and we haven't enabled the feature already, so don't continue.
					return pplx::create_task([appID]() {
						return appID;
					});
				}
			}
		}

		this->_appGroupSemaphore.wait();

		return AppleAPI::getInstance()->FetchAppGroups(team, session)
		.then([=](std::vector<std::shared_ptr<AppGroup>> fetchedGroups) {

			std::vector<pplx::task<std::shared_ptr<AppGroup>>> tasks;

			for (auto groupIdentifier : applicationGroups)
			{
				std::string adjustedGroupIdentifier = groupIdentifier + "." + team->identifier();
				std::optional<std::shared_ptr<AppGroup>> matchingGroup;

				for (auto group : fetchedGroups)
				{
					if (group->groupIdentifier() == adjustedGroupIdentifier)
					{
						matchingGroup = group;
						break;
					}
				}

				if (matchingGroup.has_value())
				{
					auto task = pplx::create_task([matchingGroup]() { return *matchingGroup; });
					tasks.push_back(task);
				}
				else
				{
					std::string name = std::regex_replace("AltStore " + groupIdentifier, std::regex("\\."), " ");

					auto task = AppleAPI::getInstance()->AddAppGroup(name, adjustedGroupIdentifier, team, session);
					tasks.push_back(task);
				}				
			}

			return pplx::when_all(tasks.begin(), tasks.end()).then([=](pplx::task<std::vector<std::shared_ptr<AppGroup>>> task) {
				try
				{
					auto groups = task.get();
					//observe_all_exceptions<std::shared_ptr<AppGroup>>(tasks.begin(), tasks.end());
					return groups;
				}
				catch (std::exception& e)
				{
					//observe_all_exceptions<std::shared_ptr<AppGroup>>(tasks.begin(), tasks.end());
					throw;
				}
			});
		})
		.then([=](std::vector<std::shared_ptr<AppGroup>> groups) {
			return AppleAPI::getInstance()->AssignAppIDToGroups(appID, groups, team, session);
		})
		.then([appID](bool result) {
			return appID;
		})
		.then([this](pplx::task<std::shared_ptr<AppID>> task) {
			this->_appGroupSemaphore.notify();

			auto appID = task.get();
			return appID;
		});
	});
}

pplx::task<std::shared_ptr<Device>> AltServerApp::RegisterDevice(std::shared_ptr<Device> device, std::shared_ptr<Team> team, std::shared_ptr<AppleAPISession> session)
{
    auto task = AppleAPI::getInstance()->FetchDevices(team, device->type(), session)
    .then([device, team, session](std::vector<std::shared_ptr<Device>> devices)
          {
              std::shared_ptr<Device> matchingDevice = nullptr;
              
              for (auto tempDevice : devices)
              {
				  odslog("Comparing  device: " << tempDevice << " (" << tempDevice->identifier() << ")  with " << device << " (" << device->identifier() << ")");
                  if (tempDevice->identifier() == device->identifier())
                  {
                      matchingDevice = tempDevice;
                      break;
                  }
              }
              
              if (matchingDevice != nullptr)
              {
                  return pplx::task<std::shared_ptr<Device>>([matchingDevice]()
                                                             {
                                                                 return matchingDevice;
                                                             });
              }
              else
              {
                  return AppleAPI::getInstance()->RegisterDevice(device->name(), device->identifier(), device->type(), team, session);
              }
          });
    
    return task;
}

pplx::task<std::shared_ptr<ProvisioningProfile>> AltServerApp::FetchProvisioningProfile(std::shared_ptr<AppID> appID, std::shared_ptr<Device> device, std::shared_ptr<Team> team, std::shared_ptr<AppleAPISession> session)
{
    return AppleAPI::getInstance()->FetchProvisioningProfile(appID, device->type(), team, session);
}

pplx::task<std::shared_ptr<Application>> AltServerApp::InstallApp(std::shared_ptr<Application> app,
                            std::shared_ptr<Device> device,
                            std::shared_ptr<Team> team,
                            std::shared_ptr<Certificate> certificate,
                            std::map<std::string, std::shared_ptr<ProvisioningProfile>> profilesByBundleID)
{
	auto prepareInfoPlist = [profilesByBundleID](std::shared_ptr<Application> app, plist_t additionalValues){
		auto profile = profilesByBundleID.at(app->bundleIdentifier());

		fs::path infoPlistPath(app->path());
		infoPlistPath.append("Info.plist");

		auto data = readFile(infoPlistPath.string().c_str());

		plist_t plist = nullptr;
		plist_from_memory((const char*)data.data(), (int)data.size(), &plist);
		if (plist == nullptr)
		{
			throw InstallError(InstallErrorCode::MissingInfoPlist);
		}

		plist_dict_set_item(plist, "CFBundleIdentifier", plist_new_string(profile->bundleIdentifier().c_str()));
		plist_dict_set_item(plist, "ALTBundleIdentifier", plist_new_string(app->bundleIdentifier().c_str()));

		if (additionalValues != NULL)
		{
			plist_dict_merge(&plist, additionalValues);
		}

		plist_t entitlements = profile->entitlements();
		if (entitlements != nullptr)
		{
			plist_t appGroups = plist_copy(plist_dict_get_item(entitlements, "com.apple.security.application-groups"));
			plist_dict_set_item(plist, "ALTAppGroups", appGroups);
		}

		char* plistXML = nullptr;
		uint32_t length = 0;
		plist_to_xml(plist, &plistXML, &length);

		std::ofstream fout(infoPlistPath.string(), std::ios::out | std::ios::binary);
		fout.write(plistXML, length);
		fout.close();
	};

    return pplx::task<std::shared_ptr<Application>>([=]() {
        fs::path infoPlistPath(app->path());
        infoPlistPath.append("Info.plist");
        
		odslog("Signing: Reading InfoPlist...");
        auto data = readFile(infoPlistPath.string().c_str());
        
        plist_t plist = nullptr;
        plist_from_memory((const char *)data.data(), (int)data.size(), &plist);
        if (plist == nullptr)
        {
            throw InstallError(InstallErrorCode::MissingInfoPlist);
        }
        
		plist_t additionalValues = plist_new_dict();

		std::string openAppURLScheme = "altstore-" + app->bundleIdentifier();

		plist_t allURLSchemes = plist_dict_get_item(plist, "CFBundleURLTypes");
		if (allURLSchemes == nullptr)
		{
			allURLSchemes = plist_new_array();
		}
		else
		{
			allURLSchemes = plist_copy(allURLSchemes);
		}

		plist_t altstoreURLScheme = plist_new_dict();
		plist_dict_set_item(altstoreURLScheme, "CFBundleTypeRole", plist_new_string("Editor"));
		plist_dict_set_item(altstoreURLScheme, "CFBundleURLName", plist_new_string(app->bundleIdentifier().c_str()));

		plist_t schemesNode = plist_new_array();
		plist_array_append_item(schemesNode, plist_new_string(openAppURLScheme.c_str()));
		plist_dict_set_item(altstoreURLScheme, "CFBundleURLSchemes", schemesNode);

		plist_array_append_item(allURLSchemes, altstoreURLScheme);
		plist_dict_set_item(additionalValues, "CFBundleURLTypes", allURLSchemes);

		if (app->isAltStoreApp())
		{
			plist_dict_set_item(additionalValues, "ALTDeviceID", plist_new_string(device->identifier().c_str()));

			auto serverID = this->serverID();
			plist_dict_set_item(additionalValues, "ALTServerID", plist_new_string(serverID.c_str()));

			auto machineIdentifier = certificate->machineIdentifier();
			if (machineIdentifier.has_value())
			{
				auto encryptedData = certificate->encryptedP12Data(*machineIdentifier);
				if (encryptedData.has_value())
				{
					plist_dict_set_item(additionalValues, "ALTCertificateID", plist_new_string(certificate->serialNumber().c_str()));

					// Embed encrypted certificate in app bundle.
					fs::path certificatePath(app->path());
					certificatePath.append("ALTCertificate.p12");

					std::ofstream fout(certificatePath.string(), std::ios::out | std::ios::binary);
					fout.write((const char*)encryptedData->data(), encryptedData->size());
					fout.close();
				}
			}
		}        

		odslog("Signing: Preparing InfoPlist...");
		prepareInfoPlist(app, additionalValues);

		for (auto appExtension : app->appExtensions())
		{
			odslog("Signing: Preparing InfoPlist for extensions...");
			prepareInfoPlist(appExtension, NULL);
		}

		odslog("Signing: Preparing provisioning profiles...");
		std::vector<std::shared_ptr<ProvisioningProfile>> profiles;
		std::set<std::string> profileIdentifiers;
		for (auto pair : profilesByBundleID)
		{
			profiles.push_back(pair.second);
			profileIdentifiers.insert(pair.second->bundleIdentifier());
		}
        
		odslog("Signing: Signing app...");
        Signer signer(team, certificate);
        signer.SignApp(app->path(), profiles);

		std::optional<std::set<std::string>> activeProfiles = std::nullopt;
		if (team->type() == Team::Type::Free && app->isAltStoreApp())
		{
			activeProfiles = profileIdentifiers;
		}
        
		odslog("Signing: Installing app...");
		return DeviceManager::instance()->InstallApp(app->path(), device->identifier(), activeProfiles, [](double progress) {
			odslog("Installation Progress: " << progress);
		})
		.then([app] {
			return app;
		});
    });
}

void AltServerApp::ShowNotification(std::string title, std::string message)
{
	std::cout << "Notify: " << title << std::endl << "    " << message << std::endl;
}

void AltServerApp::ShowAlert(std::string title, std::string message)
{
	std::cout << "Alert: " << title << std::endl << "    " << message << std::endl;
	std::cout << "Press any key to continue..." << std::endl;
	char a;
	std::cin >> a;
}

void AltServerApp::ShowInstallationNotification(std::string appName, std::string deviceName)
{
	std::stringstream ssTitle;
	ssTitle << "Installing " << appName << " to " << deviceName << "...";

	std::stringstream ssMessage;
	ssMessage << "This may take a few seconds.";

	this->ShowNotification(ssTitle.str(), ssMessage.str());
}

HWND AltServerApp::windowHandle() const
{
	return _windowHandle;
}

HINSTANCE AltServerApp::instanceHandle() const
{
	return _instanceHandle;
}

std::string AltServerApp::serverID() const
{
	return "1234567";
}

fs::path AltServerApp::appDataDirectoryPath() const
{
	fs::path altserverDirectoryPath("./AltServerData");

	if (!fs::exists(altserverDirectoryPath))
	{
		fs::create_directory(altserverDirectoryPath);
	}

	return altserverDirectoryPath;
}

fs::path AltServerApp::certificatesDirectoryPath() const
{
	auto appDataPath = this->appDataDirectoryPath();
	auto certificatesDirectoryPath = appDataPath.append("Certificates");

	if (!fs::exists(certificatesDirectoryPath))
	{
		fs::create_directory(certificatesDirectoryPath);
	}

	return certificatesDirectoryPath;
}
