#include <chrono>
#include <format>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <windows.h>
#include <winsock2.h>


#include "CClientAPI.h"
#include <optional>
#include <sstream>

using namespace std::chrono_literals;

#define PORT 8080
#define BUFFER_SIZE 1024
#define KEY "E4QPL-U27JY-J9W89-BRY7M-BGPC4"
#define PLATFORM_ADDRES "inproc://capsule"

// objects for establishing a connection with the device and capsule
clCClient client = nullptr;
clCSession session = nullptr;
clCNFBCalibrator calibrator = nullptr;
clCDeviceLocator locator = nullptr;
clCDevice device = nullptr;
std::ofstream dataFile;
// object for processing raw EEG signal and receiving NFB rhythms
clCNFB nfb = nullptr;
// object for processing the user's EEG rhythms and obtaining productivity score
// and other data
clCNFBMetricProductivity productivity = nullptr;
// object for processing the user's ECG rhythms
clCCardio cardio = nullptr;
// object for processing the user's MEMS
clCMEMS mems = nullptr;

uint32_t s_time = 0;
uint32_t deviceConnectionTime = 0;

WSADATA wsaData;
SOCKET clientSocket;

bool calibrationFailed = false;

enum LogLevel { INFO, DEBUG, WARNING, BUG, CRITICAL };

class Logger {
public:
  // Constructor: Opens the log file in append mode
  Logger(const std::string &filename) {
    logFile.open(filename, std::ios::app);
    if (!logFile.is_open()) {
      std::cerr << "Error opening log file." << std::endl;
    }
  }

  // Destructor: Closes the log file
  ~Logger() { logFile.close(); }

  // Logs a message with a given log level
  void log(LogLevel level, const std::string &message) {
    // Get current timestamp using std::chrono
    auto now = std::chrono::system_clock::now();
    time_t now_time = std::chrono::system_clock::to_time_t(now);
    tm *timeinfo = localtime(&now_time);
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);

    // Create log entry
    std::ostringstream logEntry;
    logEntry << "[" << timestamp << "] " << levelToString(level) << ": "
             << message << std::endl;

    // Output to console
    if (level == INFO || level == CRITICAL) {
      std::cerr << logEntry.str();
    }

    // Output to log file
    if (logFile.is_open()) {
      logFile << logEntry.str();
      logFile.flush(); // Ensure immediate write to file
    }
  }

private:
  std::ofstream logFile; // File stream for the log file

  // Converts log level to a string for output
  std::string levelToString(LogLevel level) {
    switch (level) {
    case INFO:
      return "INFO";
    case DEBUG:
      return "DEBUG";
    case WARNING:
      return "WARNING";
    case BUG:
      return "BUG";
    case CRITICAL:
      return "CRITICAL";
    default:
      return "UNKNOWN";
    }
  }
};

Logger logger("logfile.txt");

void send_buffer_on_server(const char *buffer) {
  int result;
  struct sockaddr_in serverAddr;

  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    logger.log(BUG, "WSAStartup failed: " + std::to_string(WSAGetLastError()));
  }

  clientSocket = socket(AF_INET, SOCK_STREAM, 0);
  if (clientSocket == INVALID_SOCKET) {
    logger.log(BUG,
               "Socket creation failed: " + std::to_string(WSAGetLastError()));
    WSACleanup();
  }

  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons(PORT);
  serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

  if (connect(clientSocket, (struct sockaddr *)&serverAddr,
              sizeof(serverAddr)) < 0) {
    logger.log(BUG, "Connection failed: " + std::to_string(WSAGetLastError()));
    closesocket(clientSocket);
    WSACleanup();
  }

  logger.log(DEBUG, "Sending buffer to server: " + std::string(buffer));
  result = send(clientSocket, buffer, strlen(buffer), 0);
  if (result == SOCKET_ERROR) {
    logger.log(BUG, "Send failed: " + std::to_string(WSAGetLastError()));
  }

  closesocket(clientSocket);
  WSACleanup();
}
/*
void openFile(const std::string &filename) {
  dataFile.open(filename, std::ios::out | std::ios::app);
  if (!dataFile) {
    std::cout << "Error opening file!" << std::endl;
  }
}

void writeToFile(const std::string &line) {
  if (dataFile.is_open()) {
    dataFile << line << std::endl;
  } else {
    std::cout << "File close!" << std::endl;
  }
}

void closeFile() {
  if (dataFile.is_open()) {
    dataFile.close();
  }
} */

// License key
std::string key;

void onResistances([[maybe_unused]] clCDevice device,
                   clCResistances resistances) {
  // Get total number of resistance channels.
  const int32_t count = clCResistances_GetCount(resistances);
  logger.log(DEBUG, "Resistances " + std::to_string(count));
  for (int32_t i = 0; i < count; ++i) {
    const char *channelName =
        clCString_CStr(clCResistances_GetChannelName(resistances, i));
    const float value = clCResistances_GetValue(resistances, i);
    logger.log(DEBUG,
               "\t" + std::string(channelName) + " = " + std::to_string(value));
  }
}

void onNFBInitializedEvent(clCNFB nfb) {
  // Еrain NFB on valid data
  clCNFB_Train(nfb);
}

void onUpdateUserState([[maybe_unused]] clCNFB nfb,
                       const clCNFBUserState *userState) {
  // Getting NFB user data
  // if artifacts or weak resistance on the electrodes are observed,
  // the data will not be changed
  logger.log(DEBUG,
             "NFB update state: alpha = " +
                 std::to_string(userState->feedbackData[0]) +
                 " , beta = " + std::to_string(userState->feedbackData[1]) +
                 " , theta = " + std::to_string(userState->feedbackData[2]));
}

void onNFBErrorEvent([[maybe_unused]] clCNFB nfb, const char *error) {
  logger.log(DEBUG, "NFB error: " + std::string(error));
}

void onNFBTrainEvent([[maybe_unused]] clCNFB nfb) {
  // After training nfb - subscribe to the data that we are interested in
  logger.log(DEBUG, "NFB train\n");
  const clCNFBCallResult resultAlpha = clCNFB_AddFeedbackFunction(nfb, "alpha");
  const clCNFBCallResult resultBeta = clCNFB_AddFeedbackFunction(nfb, "beta");
  const clCNFBCallResult resultTheta = clCNFB_AddFeedbackFunction(nfb, "theta");

  if (resultAlpha == clC_NFB_Success && resultBeta == clC_NFB_Success &&
      resultTheta == clC_NFB_Success) {
    logger.log(DEBUG, "call addfeedback success\n");
  }
}

void onProductivityArtifacts(
    [[maybe_unused]] clCNFBMetricProductivity productivity,
    [[maybe_unused]] const clCNFBUserArtifacts *) {
  logger.log(DEBUG, "productivity getting information about artifacts\n");
}

void onProductivityIndividualInfo(
    [[maybe_unused]] clCNFBMetricProductivity productivity,
    [[maybe_unused]] const clCNFBMetricsProductivityIndividualIndexes *) {
  logger.log(DEBUG, "productivity getting individual information about user\n");
}

void onProductivtyScoreUpdate(
    [[maybe_unused]] clCNFBMetricProductivity productivity,
    float productivityScore) {
  logger.log(DEBUG, "productivity getting productivity score: " +
                        std::to_string(productivityScore));
}

void onProductivityValuesUpdate(clCNFBMetricProductivity,
                                const clCNFBMetricsProductivityValues *values) {
  logger.log(
      DEBUG,
      "Productivity values update:\n\tFatigue Score: " +
          std::to_string(values->fatigueScore) + '\n' +
          "\tGravity Score: " + std::to_string(values->gravityScore) + '\n' +
          "\tConcentration Score: " +
          std::to_string(values->concentrationScore) + '\n' +
          "\tRelaxation Score: " + std::to_string(values->relaxationScore) +
          '\n' + "\tAccumulated Fatigue: " +
          std::to_string(values->accumulatedFatigue) + '\n' +
          "\tFatigue Growth Rate: " +
          std::to_string(values->fatigueGrowthRate));
}

void onCardioIndexesUpdate([[maybe_unused]] clCCardio cardio,
                           clCCardioData data) {
  logger.log(DEBUG, "Cardio index update: (artifacted " +
                        std::to_string(data.artifacted) + "), Kaplan's index " +
                        std::to_string(data.kaplanIndex) + ", HR " +
                        std::to_string(data.heartRate) + ", stress index " +
                        std::to_string(data.stressIndex));
}

void onMEMSUpdate([[maybe_unused]] clCMEMS mems, clCMEMSTimedData data) {
  const int32_t count = clCMEMSTimedData_GetCount(data);
  logger.log(DEBUG,
             "MEMS update: showing 1 of " + std::to_string(count) + " values");
  const clCPoint3d accelerometer = clCMEMSTimedData_GetAccelerometer(data, 0);
  const clCPoint3d gyroscope = clCMEMSTimedData_GetGyroscope(data, 0);
  const auto timepoint = clCMEMSTimedData_GetTimepoint(data, 0);
  std::stringstream line_stream;
  line_stream << "MEMS" << " " << timepoint << " " << accelerometer.x << " "
              << accelerometer.y << " " << accelerometer.y << " " << gyroscope.x
              << " " << gyroscope.y << " " << gyroscope.z;
  std::string line = line_stream.str();
  logger.log(DEBUG, line);
  char buf[BUFFER_SIZE];
  strcpy(buf, line.c_str());
  send_buffer_on_server((char *)buf);
}

void onCalibrated(clCNFBCalibrator, const clCIndividualNFBData *data,
                  clCIndividualNFBCalibrationFailReason failReason) {
  if (data == nullptr ||
      failReason != clC_IndividualNFBCalibrationFailReason_None) {
    logger.log(INFO, "Calibration failed");
    if (failReason == clC_IndividualNFBCalibrationFailReason_TooManyArtifacts) {
      logger.log(INFO, "Calibration failed: Too many artifacts");
    } else if (failReason ==
               clC_IndividualNFBCalibrationFailReason_PeakIsABorder) {
      logger.log(INFO, ": Alpha peak matches one of the alpha tange borders");
    } else {
      logger.log(INFO, ": Reason unknown");
    }
    calibrationFailed = true;
    return;
  }
  logger.log(INFO, "Calibrated: " + std::to_string(data->individualFrequency));
}

void onCalibratorReady(clCNFBCalibrator calibrator) {
  logger.log(
      INFO,
      "Calibrator is ready to calibrate NFB\nClose your eyes for 30 seconds");
  clCError error = clC_Error_OK;
  clCNFBCalibrator_CalibrateIndividualNFBQuick(calibrator, &error);
}

void onEmotionFocusUpdate(clCEmotions emotionFocus) {
  logger.log(INFO, "Emotion focus update: " + std::to_string(emotionFocus));
}

std::optional<bool> licenseVerified;
void onLicenseVerified(clCLicenseManager, bool result, clCLicenseError) {
  logger.log(INFO, "License verified: " + std::to_string(result));
  licenseVerified.emplace(result);
}

void onSessionStarted(clCSession session) {
  logger.log(INFO, "Session started");
  const char *sessionUUID = clCString_CStr(clCSession_GetSessionUUID(session));
  logger.log(INFO, "Session UUID: " + std::string(sessionUUID));
  clCSession_MarkActivity(session, clCUserActivity1);

  calibrator = clCNFBCalibrator_CreateOrGet(session);
  clCNFBCalibratorDelegateIndividualNFBCalibrated onCalibratedEvent =
      clCNFBCalibrator_GetOnIndividualNFBCalibratedEvent(calibrator);
  clCNFBCalibratorDelegateIndividualNFBCalibrated_Set(onCalibratedEvent,
                                                      onCalibrated);
  clCNFBCalibratorDelegateReadyToCalibrate onCalibratorReadyEvent =
      clCNFBCalibrator_GetOnReadyToCalibrate(calibrator);
  clCNFBCalibratorDelegateReadyToCalibrate_Set(onCalibratorReadyEvent,
                                               onCalibratorReady);

  // Create NFB - NeiryFeedBack
  nfb = clCNFB_Create(session);
  // get NFB events
  clCNFBDelegate delegateInit = clCNFB_GetOnInitializedEvent(nfb);
  clCNFBDelegateNFBUserState delegateState =
      clCNFB_GetOnUserStateChangedEvent(nfb);
  clCNFBDelegateString delegateError = clCNFB_GetOnErrorEvent(nfb);
  clCNFBDelegate delegateTrain = clCNFB_GetOnModelTrainedEvent(nfb);
  // Initialize NFB events
  clCNFBDelegate_Set(delegateInit, onNFBInitializedEvent);
  clCNFBDelegateNFBUserState_Set(delegateState, onUpdateUserState);
  clCNFBDelegateString_Set(delegateError, onNFBErrorEvent);
  clCNFBDelegate_Set(delegateTrain, onNFBTrainEvent);

  // Initialize NFB
  clCNFB_Initialize(nfb);

  // Productivity metrics is an algorithm that uses custom rhythms
  // and an IAPF and other indicators to calculate the productivity score
  // Create Productivity metric - math constants are better not to change yet
  clCError error = clC_Error_OK;
  productivity = clCNFBMetricsProductivity_Create(session, "logs", 0.05, 0.05,
                                                  0.001, &error);
  if (error != clC_Error_OK) {
    logger.log(BUG, "Failed to create Productivity classifier");
    return;
  }
  // get Productivity events
  clCNFBMetricsProductivityIndividualDelegate delegateMeasured =
      clCNFBMetricsProductivity_GetOnIndividualMeasuredEvent(productivity);
  clCNFBMetricsProductivityArtifactsDelegate delegateArtifacts =
      clCNFBMetricsProductivity_GetOnArtifactsEvent(productivity);
  clCNFBMetricsProductivityEventDelegate delegateUserUpdate =
      clCNFBMetricsProductivity_GetOnUpdateEvent_1min(productivity);
  clCNFBMetricsProductivityValuesDelegate delegateValuesUpdate =
      clCNFBMetricsProductivity_GetOnProductivityValuesEvent(productivity);
  // Initialize Productivity events
  clCNFBMetricsProductivity_IndividualMeasuredEvent_Set(
      delegateMeasured, onProductivityIndividualInfo);
  clCNFBMetricsProductivity_ArtifactsEvent_Set(delegateArtifacts,
                                               onProductivityArtifacts);
  clCNFBMetricsProductivity_UpdateEvent_Set(delegateUserUpdate,
                                            onProductivtyScoreUpdate);
  clCNFBMetricsProductivity_ValuesEvent_Set(delegateValuesUpdate,
                                            onProductivityValuesUpdate);
  // Initialize Productivity
  clCNFBMetricsProductivity_InitializeNFB(productivity, "", &error);

  clCDevice_SwitchMode(device, clC_DM_Signal);
  cardio = clCCardio_Create(session);
  if (cardio == nullptr) {
    logger.log(BUG, "Failed to create cardio classifier");
    return;
  }
  clCCardioIndexesDelegate delegateCardio =
      clCCardio_GetOnIndexesUpdateEvent(cardio);
  clCCardioDelegateIndexesUpdate_Set(delegateCardio, onCardioIndexesUpdate);
  clCCardio_Initialize(cardio);
  clCDevice_SwitchMode(device, clC_DM_StartPPG);

  mems = clCMEMS_Create(session);
  if (mems == nullptr) {
    logger.log(BUG, "Failed to create MEMS classifier");
    return;
  }
  clCMEMSTimedDataDelegate delegateMEMS =
      clCMEMS_GetOnMEMSTimedDataUpdateEvent(mems);
  clCMEMSDelegateMEMSTimedDataUpdate_Set(delegateMEMS, onMEMSUpdate);
  clCMEMS_Initialize(mems);

  clCDevice_SwitchMode(device, clC_DM_StartMEMS);

  clCEmotions emotions = clCEmotions_CreateCalibrated(calibrator, 0.05, 0.05);

  clCEmotionsDelegate initEmotionsDelegate =
      clCEmotions_GetOnInitializedEvent(emotions);
  clCEmotionsDelegateEmotionalStatesUpdate onEmotionFocusUpdate =
      clCEmotions_GetOnEmotionalStatesUpdateEvent(emotions);

  clCEmotionsDelegate calibrate = clCEmotions_GetOnCalibratedEvent(emotions);
  clCEmotionsDelegate_Set()

      clCEmotionsDelegateFloat focusDelegate =
          clCEmotions_GetOnEmotionUpdateEvent(emotions,
                                              clC_Emotions_EmotionFocus);
  clCEmotionsDelegateFloat chillDelegate =
      clCEmotions_GetOnEmotionUpdateEvent(emotions, clC_Emotions_EmotionChill);
  clCEmotionsDelegateFloat stressDelegate =
      clCEmotions_GetOnEmotionUpdateEvent(emotions, clC_Emotions_EmotionStress);

  clCEmotionsDelegateFloat_Set(focusDelegate, onEmotionFocusUpdate);
  clCEmotionsDelegateFloat_Set(chillDelegate, onEmotionChillUpdate);
  clCEmotionsDelegateFloat_Set(stressDelegate, onEmotionStressUpdate);
  clCEmotions_Initialize(emotions);
}

void onSessionError([[maybe_unused]] clCSession session,
                    clCSessionError error) {
  logger.log(BUG, "Session error: " + std::to_string(error));
}

void onSessionStopped([[maybe_unused]] clCSession session) {
  logger.log(INFO, "Session stopped");
}

void onConnectionStateChanged([[maybe_unused]] clCDevice device,
                              clCDeviceConnectionState state) {
  // status of the device changed
  if (state != clC_SE_Connected) {
    // TODO : destroy session and get new
    logger.log(INFO, "Device disconnected. Trying connected again");
    clCSession_Stop(session);
    clCDevice_Connect(device);
    session = nullptr;
    return;
  }
  logger.log(INFO, "Device connected");
  deviceConnectionTime = s_time;

  // get channel names
  clCDeviceChannelNames channelNames = clCDevice_GetChannelNames(device);
  const auto channelsCount = clCDevice_GetChannelsCount(channelNames);
  logger.log(INFO, "Channels count: " + std::to_string(channelsCount));
  for (int32_t i = 0; i < channelsCount; ++i) {
    logger.log(INFO,
               "Channel name: " +
                   std::string(clCString_CStr(
                       clCDevice_GetChannelNameByIndex(channelNames, i))));
  }
  for (int32_t i = 0; i < channelsCount; ++i) {
    const char *channel =
        clCString_CStr(clCDevice_GetChannelNameByIndex(channelNames, i));
    logger.log(INFO, "Channel index: " +
                         std::to_string(clCDevice_GetChannelIndexByName(
                             channelNames, channel)));
  }

  auto licenseManager = clCClient_GetLicenseManager(client);
  auto onLicenseVerifiedEvent =
      clCLicenseManager_GetOnLicenseVerifiedEvent(licenseManager);
  clCLicenseManagerDelegateLicenseVerified_Set(onLicenseVerifiedEvent,
                                               onLicenseVerified);
  clCLicenseManager_VerifyLicense(licenseManager, key.c_str(), device);
  while (!licenseVerified.has_value()) {
    clCClient_Update(client);
    std::this_thread::sleep_for(std::chrono::seconds{1});
  }
}

void onDeviceList(clCDeviceLocator locator, clCDeviceInfoList devices,
                  clCDeviceLocatorFailReason error) {
  // device connected
  if (device != nullptr) {
    return;
  }

  switch (error) {
  case clC_DeviceLocatorFailReason_OK:
    break;
  case clC_DeviceLocatorFailReason_BluetoothDisabled:
    logger.log(BUG, "Bluetooth adapter not found or disabled");
    return;
  case clC_DeviceLocatorFailReason_Unknown:
    logger.log(BUG, "Unknown error occurred");
    return;
  default:
    logger.log(BUG, "Unknown DeviceLocatorFailReason value");
    return;
  }

  // number of detected devices = 0 -> start the search again
  if (clCDeviceInfoList_GetCount(devices) == 0) {
    logger.log(INFO, "No devices found");
    clCDeviceLocator_RequestDevices(locator, 10);
    return;
  }

  // print information about all found devices
  const int32_t count = clCDeviceInfoList_GetCount(devices);
  logger.log(INFO, "Devices found: " + std::to_string(count));
  for (int i = 0; i < count; i++) {
    clCDeviceInfo deviceDescriptor =
        clCDeviceInfoList_GetDeviceInfo(devices, i);
    const char *deviceDescription =
        clCString_CStr(clCDeviceInfo_GetDescription(deviceDescriptor));
    logger.log(INFO, "Device: " + std::string(deviceDescription));
  }

  // select device and connect
  clCDeviceInfo deviceDescriptor = clCDeviceInfoList_GetDeviceInfo(devices, 0);
  const char *deviceID = clCString_CStr(clCDeviceInfo_GetID(deviceDescriptor));
  device = clCDeviceLocator_CreateDevice(locator, deviceID);
  if (device == nullptr) {
    logger.log(BUG, "Failed to create device");
    return;
  }
  // Get device events
  clCDeviceDelegateResistances onResistancesEvent =
      clCDevice_GetOnResistancesEvent(device);
  clCDeviceDelegateDeviceConnectionState onConnectionStateChangedEvent =
      clCDevice_GetOnConnectionStateChangedEvent(device);
  //  Initialize device events
  clCDeviceDelegateResistances_Set(onResistancesEvent, onResistances);
  clCDeviceDelegateConnectionState_Set(onConnectionStateChangedEvent,
                                       onConnectionStateChanged);
  //  Сonnect to the device
  clCDevice_Connect(device);
}

void onConnected(clCClient client) {
  // As soon as the capsule has confirmed the connection,
  // launch the locator to search for the selected device.
  // when receiving a list of available devices, control is transferred to
  // onDeviceList
  logger.log(INFO, "Client connected");
  locator = clCClient_ChooseDeviceType(client, clC_DT_NeiryBand);
  clCDeviceLocatorDelegateDeviceInfoList onDevicesEvent =
      clCDeviceLocator_GetOnDevicesEvent(locator);
  clCDeviceLocatorDelegateDeviceInfoList_Set(onDevicesEvent, onDeviceList);
  clCDeviceLocator_RequestDevices(locator, 15);
}

void onError([[maybe_unused]] clCClient client, clCError error) {
  logger.log(BUG, "Error: " + std::to_string(error));
}

void onDisconnected(clCClient client, clCDisconnectReason reason) {
  // destroy all objects
  logger.log(INFO, "Disconnected" + std::to_string(static_cast<int>(reason)));

  if (mems) {
    clCMEMS_Destroy(mems);
    mems = nullptr;
  }
  if (cardio) {
    clCCardio_Destroy(cardio);
    cardio = nullptr;
  }
  if (nfb) {
    clCNFB_Destroy(nfb);
    nfb = nullptr;
  }
  if (productivity) {
    clCNFBMetricsProductivity_Destroy(productivity);
    productivity = nullptr;
  }
  calibrator = nullptr;
  if (session) {
    clCSession_Destroy(session);
    session = nullptr;
  }
  if (device) {
    clCDevice_Release(device);
    device = nullptr;
  }
  clCDeviceLocator_Destroy(locator);
  locator = nullptr;
  clCClient_Destroy(client);
  ::client = nullptr;
  logger.log(DEBUG, "End work");
}

bool clientStopRequested = false;
void ClientLoop() {
  static constexpr uint32_t kMsSec = 1000U;
  s_time = 0;
  while (client && !clientStopRequested) {
    try { // Update does all the work and must be called regularly to process
          // events.
      clCClient_Update(client);
      std::this_thread::sleep_for(50ms);
      s_time += 50;
      // Create a session object. The session is necessary for
      // the client to work with the device - obtaining user state data
      // or information about the device
      if (deviceConnectionTime && s_time == deviceConnectionTime + 2 * kMsSec &&
          !session) {
        // Create session
        auto error = clC_Error_OK;
        session = clCClient_CreateSessionWithError(client, device, &error);

        // Get session events
        clCSessionDelegate onSessionStartedEvent =
            clCSession_GetOnSessionStartedEvent(session);
        clCSessionDelegateSessionError onSessionErrorEvent =
            clCSession_GetOnErrorEvent(session);
        clCSessionDelegate onSessionStoppedEvent =
            clCSession_GetOnSessionStoppedEvent(session);

        // Initialize session events
        clCSessionDelegate_Set(onSessionStartedEvent, onSessionStarted);
        clCSessionDelegateSessionError_Set(onSessionErrorEvent, onSessionError);
        clCSessionDelegate_Set(onSessionStoppedEvent, onSessionStopped);

        // Start session
        const clCSessionState state = clCSession_GetSessionState(session);
        clCSession_Start(session);
        logger.log(INFO,
                   "Session state: " + std::to_string(static_cast<int>(state)));
      }
      if (!session) {
        // Destroy all objects that were created at the end of the program and
        // break the connection in callback onDisconnected
        if (s_time == 100 * kMsSec) {
          clCClient_Disconnect(client);
        }
        continue;
      }
      if (static bool printFirmware = true;
          printFirmware && clCDevice_FirmwareVersionReceived(device)) {
        clCError error = clCError::clC_Error_OK;
        const auto firmware = clCDevice_GetFirmwareVersion(device, &error);
        logger.log(INFO,
                   "Device firmware: " + std::string(clCString_CStr(firmware)));
        clCString_Free(firmware);
        printFirmware = false;
      }
    } catch (const std::exception &e) {
      logger.log(CRITICAL, "Exception: " + std::string(e.what()));
    }
  }

  if (client) {
    clCClient_Disconnect(client);
  }

  exit(0);
}

int main() {

  // License Key
  /* std::cout << "Enter your license key:\n"; */
  key = KEY;

  // Getting the version of the library
  // and an example of working with a clCString
  {
    clCString strPtr = clCClient_GetVersionString();
    logger.log(INFO, ("version of the library: " +
                      std::string(clCString_CStr(strPtr))));
    clCString_Free(strPtr);
  }

  // Create client
  client = clCClient_CreateWithName("CapsuleClientExample");

  // Getting the client name of the library
  // and an example of working with a clCString
  {
    clCString strPtr = clCClient_GetClientName(client);
    logger.log(INFO, ("client name of the library: " +
                      std::string(clCString_CStr(strPtr))));
    clCString_Free(strPtr);
  }

  // Get client events
  clCClientDelegate onConnectedEvent = clCClient_GetOnConnectedEvent(client);
  clCClientDelegateError onErrorEvent = clCClient_GetOnErrorEvent(client);
  clCClientDelegateDisconnectReason onDisconnectedEvent =
      clCClient_GetOnDisconnectedEvent(client);

  // Initialize client events
  clCClientDelegate_Set(onConnectedEvent, onConnected);
  clCClientDelegateError_Set(onErrorEvent, onError);
  clCClientDelegateDisconnectReason_Set(onDisconnectedEvent, onDisconnected);

  clCClient_Connect(client, "inproc://capsule");

  auto future = std::async(std::launch::async, ClientLoop);
  char input;
  while (std::cin >> input) {
    if (input == 'q' || input == 'Q') {
      clientStopRequested = true;
      break;
    }
  }
  future.wait();
  return 0;
}
