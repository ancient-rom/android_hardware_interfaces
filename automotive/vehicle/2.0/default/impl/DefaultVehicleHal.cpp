/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "DefaultVehicleHal"

#include <algorithm>
#include <android/log.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "DefaultVehicleHal.h"
#include "VehicleHalProto.pb.h"

#define DEBUG_SOCKET    (33452)

namespace android {
namespace hardware {
namespace automotive {
namespace vehicle {
namespace V2_0 {

namespace impl {

void DefaultVehicleHal::doGetConfig(emulator::EmulatorMessage& rxMsg,
                                    emulator::EmulatorMessage& respMsg) {
    std::vector<VehiclePropConfig> configs = listProperties();
    emulator::VehiclePropGet getProp = rxMsg.prop(0);

    respMsg.set_msg_type(emulator::GET_CONFIG_RESP);
    respMsg.set_status(emulator::ERROR_INVALID_PROPERTY);

    for (auto& config : configs) {
        // Find the config we are looking for
        if (config.prop == getProp.prop()) {
            emulator::VehiclePropConfig* protoCfg = respMsg.add_config();
            populateProtoVehicleConfig(protoCfg, config);
            respMsg.set_status(emulator::RESULT_OK);
            break;
        }
    }
}

void DefaultVehicleHal::doGetConfigAll(emulator::EmulatorMessage& /* rxMsg */,
                                       emulator::EmulatorMessage& respMsg) {
    std::vector<VehiclePropConfig> configs = listProperties();

    respMsg.set_msg_type(emulator::GET_CONFIG_ALL_RESP);
    respMsg.set_status(emulator::RESULT_OK);

    for (auto& config : configs) {
        emulator::VehiclePropConfig* protoCfg = respMsg.add_config();
        populateProtoVehicleConfig(protoCfg, config);
    }
}

void DefaultVehicleHal::doGetProperty(emulator::EmulatorMessage& rxMsg,
                                      emulator::EmulatorMessage& respMsg) {
    int32_t areaId = 0;
    emulator::VehiclePropGet getProp = rxMsg.prop(0);
    int32_t propId = getProp.prop();
    emulator::Status status = emulator::ERROR_INVALID_PROPERTY;
    VehiclePropValue* val;

    respMsg.set_msg_type(emulator::GET_PROPERTY_RESP);

    if (getProp.has_area_id()) {
        areaId = getProp.area_id();
    }

    {
        std::lock_guard<std::mutex> lock(mPropsMutex);

        val = getVehiclePropValueLocked(propId, areaId);
        if (val != nullptr) {
            emulator::VehiclePropValue* protoVal = respMsg.add_value();
            populateProtoVehiclePropValue(protoVal, val);
            status = emulator::RESULT_OK;
        }
    }

    respMsg.set_status(status);
}

void DefaultVehicleHal::doGetPropertyAll(emulator::EmulatorMessage& /* rxMsg */,
                                         emulator::EmulatorMessage& respMsg) {
    respMsg.set_msg_type(emulator::GET_PROPERTY_ALL_RESP);
    respMsg.set_status(emulator::RESULT_OK);

    {
        std::lock_guard<std::mutex> lock(mPropsMutex);

        for (auto& propVal : mProps) {
            emulator::VehiclePropValue* protoVal = respMsg.add_value();
            populateProtoVehiclePropValue(protoVal, propVal.get());
        }
    }
}

void DefaultVehicleHal::doSetProperty(emulator::EmulatorMessage& rxMsg,
                                      emulator::EmulatorMessage& respMsg) {
    emulator::VehiclePropValue protoVal = rxMsg.value(0);
    VehiclePropValue val;

    respMsg.set_msg_type(emulator::SET_PROPERTY_RESP);

    val.prop = protoVal.prop();
    val.areaId = protoVal.area_id();

    // Copy value data if it is set.  This automatically handles complex data types if needed.
    if (protoVal.has_string_value()) {
        val.value.stringValue = protoVal.string_value().c_str();
    }

    if (protoVal.has_bytes_value()) {
        std::vector<uint8_t> tmp(protoVal.bytes_value().begin(), protoVal.bytes_value().end());
        val.value.bytes = tmp;
    }

    if (protoVal.int32_values_size() > 0) {
        std::vector<int32_t> int32Values = std::vector<int32_t>(protoVal.int32_values_size());
        for (int i=0; i<protoVal.int32_values_size(); i++) {
            int32Values[i] = protoVal.int32_values(i);
        }
        val.value.int32Values = int32Values;
    }

    if (protoVal.int64_values_size() > 0) {
        std::vector<int64_t> int64Values = std::vector<int64_t>(protoVal.int64_values_size());
        for (int i=0; i<protoVal.int64_values_size(); i++) {
            int64Values[i] = protoVal.int64_values(i);
        }
        val.value.int64Values = int64Values;
    }

    if (protoVal.float_values_size() > 0) {
        std::vector<float> floatValues = std::vector<float>(protoVal.float_values_size());
        for (int i=0; i<protoVal.float_values_size(); i++) {
            floatValues[i] = protoVal.float_values(i);
        }
        val.value.floatValues = floatValues;
    }

    if (updateProperty(val) == StatusCode::OK) {
        // Send property up to VehicleHalManager via callback
        auto& pool = *getValuePool();
        VehiclePropValuePtr v = pool.obtain(val);

        doHalEvent(std::move(v));
        respMsg.set_status(emulator::RESULT_OK);
    } else {
        respMsg.set_status(emulator::ERROR_INVALID_PROPERTY);
    }
}

// This function should only be called while mPropsMutex is locked.
VehiclePropValue* DefaultVehicleHal::getVehiclePropValueLocked(int32_t propId, int32_t areaId) {
    if (getPropArea(propId) == VehicleArea::GLOBAL) {
        // In VehicleHal, global properties have areaId = -1.  We use 0.
        areaId = 0;
    }

    for (auto& prop : mProps) {
        if ((prop->prop == propId) && (prop->areaId == areaId)) {
            return prop.get();
        }
    }
    ALOGW("%s: Property not found:  propId = 0x%x, areaId = 0x%x", __FUNCTION__, propId, areaId);
    return nullptr;
}

void DefaultVehicleHal::initObd2LiveFrame(VehiclePropConfig& obd2LiveFramePropConfig) {
    mObd2SensorStore.reset(new Obd2SensorStore(
        obd2LiveFramePropConfig.configArray[0],
        obd2LiveFramePropConfig.configArray[1]));
    // precalculate OBD2 sensor values
    mObd2SensorStore->setIntegerSensor(
        Obd2IntegerSensorIndex::FUEL_SYSTEM_STATUS,
        toInt(FuelSystemStatus::CLOSED_LOOP));
    mObd2SensorStore->setIntegerSensor(
        Obd2IntegerSensorIndex::MALFUNCTION_INDICATOR_LIGHT_ON, 0);
    mObd2SensorStore->setIntegerSensor(
        Obd2IntegerSensorIndex::IGNITION_MONITORS_SUPPORTED,
        toInt(IgnitionMonitorKind::SPARK));
    mObd2SensorStore->setIntegerSensor(Obd2IntegerSensorIndex::IGNITION_SPECIFIC_MONITORS,
        CommonIgnitionMonitors::COMPONENTS_AVAILABLE |
        CommonIgnitionMonitors::MISFIRE_AVAILABLE |
        SparkIgnitionMonitors::AC_REFRIGERANT_AVAILABLE |
        SparkIgnitionMonitors::EVAPORATIVE_SYSTEM_AVAILABLE);
    mObd2SensorStore->setIntegerSensor(
        Obd2IntegerSensorIndex::INTAKE_AIR_TEMPERATURE, 35);
    mObd2SensorStore->setIntegerSensor(
        Obd2IntegerSensorIndex::COMMANDED_SECONDARY_AIR_STATUS,
        toInt(SecondaryAirStatus::FROM_OUTSIDE_OR_OFF));
    mObd2SensorStore->setIntegerSensor(
        Obd2IntegerSensorIndex::NUM_OXYGEN_SENSORS_PRESENT, 1);
    mObd2SensorStore->setIntegerSensor(
        Obd2IntegerSensorIndex::RUNTIME_SINCE_ENGINE_START, 500);
    mObd2SensorStore->setIntegerSensor(
        Obd2IntegerSensorIndex::DISTANCE_TRAVELED_WITH_MALFUNCTION_INDICATOR_LIGHT_ON, 0);
    mObd2SensorStore->setIntegerSensor(
        Obd2IntegerSensorIndex::WARMUPS_SINCE_CODES_CLEARED, 51);
    mObd2SensorStore->setIntegerSensor(
        Obd2IntegerSensorIndex::DISTANCE_TRAVELED_SINCE_CODES_CLEARED, 365);
    mObd2SensorStore->setIntegerSensor(
        Obd2IntegerSensorIndex::ABSOLUTE_BAROMETRIC_PRESSURE, 30);
    mObd2SensorStore->setIntegerSensor(
        Obd2IntegerSensorIndex::CONTROL_MODULE_VOLTAGE, 12);
    mObd2SensorStore->setIntegerSensor(
        Obd2IntegerSensorIndex::AMBIENT_AIR_TEMPERATURE, 18);
    mObd2SensorStore->setIntegerSensor(
        Obd2IntegerSensorIndex::MAX_FUEL_AIR_EQUIVALENCE_RATIO, 1);
    mObd2SensorStore->setIntegerSensor(
        Obd2IntegerSensorIndex::FUEL_TYPE, toInt(FuelType::GASOLINE));
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::CALCULATED_ENGINE_LOAD, 0.153);
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::SHORT_TERM_FUEL_TRIM_BANK1, -0.16);
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::LONG_TERM_FUEL_TRIM_BANK1, -0.16);
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::SHORT_TERM_FUEL_TRIM_BANK2, -0.16);
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::LONG_TERM_FUEL_TRIM_BANK2, -0.16);
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::INTAKE_MANIFOLD_ABSOLUTE_PRESSURE, 7.5);
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::ENGINE_RPM, 1250.);
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::VEHICLE_SPEED, 40.);
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::TIMING_ADVANCE, 2.5);
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::THROTTLE_POSITION, 19.75);
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::OXYGEN_SENSOR1_VOLTAGE, 0.265);
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::FUEL_TANK_LEVEL_INPUT, 0.824);
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::EVAPORATION_SYSTEM_VAPOR_PRESSURE, -0.373);
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::CATALYST_TEMPERATURE_BANK1_SENSOR1, 190.);
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::RELATIVE_THROTTLE_POSITION, 3.);
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::ABSOLUTE_THROTTLE_POSITION_B, 0.306);
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::ACCELERATOR_PEDAL_POSITION_D, 0.188);
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::ACCELERATOR_PEDAL_POSITION_E, 0.094);
    mObd2SensorStore->setFloatSensor(
        Obd2FloatSensorIndex::COMMANDED_THROTTLE_ACTUATOR, 0.024);
}

void DefaultVehicleHal::parseRxProtoBuf(std::vector<uint8_t>& msg) {
    emulator::EmulatorMessage rxMsg;
    emulator::EmulatorMessage respMsg;
    std::string str(reinterpret_cast<const char*>(msg.data()), msg.size());

    rxMsg.ParseFromString(str);

    switch (rxMsg.msg_type()) {
    case emulator::GET_CONFIG_CMD:
        doGetConfig(rxMsg, respMsg);
        break;
    case emulator::GET_CONFIG_ALL_CMD:
        doGetConfigAll(rxMsg, respMsg);
        break;
    case emulator::GET_PROPERTY_CMD:
        doGetProperty(rxMsg, respMsg);
        break;
    case emulator::GET_PROPERTY_ALL_CMD:
        doGetPropertyAll(rxMsg, respMsg);
        break;
    case emulator::SET_PROPERTY_CMD:
        doSetProperty(rxMsg, respMsg);
        break;
    default:
        ALOGW("%s: Unknown message received, type = %d", __FUNCTION__, rxMsg.msg_type());
        respMsg.set_status(emulator::ERROR_UNIMPLEMENTED_CMD);
        break;
    }

    // Send the reply
    txMsg(respMsg);
}

// Copies internal VehiclePropConfig data structure to protobuf VehiclePropConfig
void DefaultVehicleHal::populateProtoVehicleConfig(emulator::VehiclePropConfig* protoCfg,
                                                   const VehiclePropConfig& cfg) {
    protoCfg->set_prop(cfg.prop);
    protoCfg->set_access(toInt(cfg.access));
    protoCfg->set_change_mode(toInt(cfg.changeMode));
    protoCfg->set_value_type(toInt(getPropType(cfg.prop)));

    if (!isGlobalProp(cfg.prop)) {
        protoCfg->set_supported_areas(cfg.supportedAreas);
    }

    for (auto& configElement : cfg.configArray) {
        protoCfg->add_config_array(configElement);
    }

    if (cfg.configString.size() > 0) {
        protoCfg->set_config_string(cfg.configString.c_str(), cfg.configString.size());
    }

    // Populate the min/max values based on property type
    switch (getPropType(cfg.prop)) {
    case VehiclePropertyType::STRING:
    case VehiclePropertyType::BOOLEAN:
    case VehiclePropertyType::INT32_VEC:
    case VehiclePropertyType::FLOAT_VEC:
    case VehiclePropertyType::BYTES:
    case VehiclePropertyType::COMPLEX:
        // Do nothing.  These types don't have min/max values
        break;
    case VehiclePropertyType::INT64:
        if (cfg.areaConfigs.size() > 0) {
            emulator::VehicleAreaConfig* aCfg = protoCfg->add_area_configs();
            aCfg->set_min_int64_value(cfg.areaConfigs[0].minInt64Value);
            aCfg->set_max_int64_value(cfg.areaConfigs[0].maxInt64Value);
        }
        break;
    case VehiclePropertyType::FLOAT:
        if (cfg.areaConfigs.size() > 0) {
            emulator::VehicleAreaConfig* aCfg = protoCfg->add_area_configs();
            aCfg->set_min_float_value(cfg.areaConfigs[0].minFloatValue);
            aCfg->set_max_float_value(cfg.areaConfigs[0].maxFloatValue);
        }
        break;
    case VehiclePropertyType::INT32:
        if (cfg.areaConfigs.size() > 0) {
            emulator::VehicleAreaConfig* aCfg = protoCfg->add_area_configs();
            aCfg->set_min_int32_value(cfg.areaConfigs[0].minInt32Value);
            aCfg->set_max_int32_value(cfg.areaConfigs[0].maxInt32Value);
        }
        break;
    default:
        ALOGW("%s: Unknown property type:  0x%x", __FUNCTION__, toInt(getPropType(cfg.prop)));
        break;
    }

    protoCfg->set_min_sample_rate(cfg.minSampleRate);
    protoCfg->set_max_sample_rate(cfg.maxSampleRate);
}

// Copies internal VehiclePropValue data structure to protobuf VehiclePropValue
void DefaultVehicleHal::populateProtoVehiclePropValue(emulator::VehiclePropValue* protoVal,
                                                      const VehiclePropValue* val) {
    protoVal->set_prop(val->prop);
    protoVal->set_value_type(toInt(getPropType(val->prop)));
    protoVal->set_timestamp(val->timestamp);
    protoVal->set_area_id(val->areaId);

    // Copy value data if it is set.
    //  - for bytes and strings, this is indicated by size > 0
    //  - for int32, int64, and float, copy the values if vectors have data
    if (val->value.stringValue.size() > 0) {
        protoVal->set_string_value(val->value.stringValue.c_str(), val->value.stringValue.size());
    }

    if (val->value.bytes.size() > 0) {
        protoVal->set_bytes_value(val->value.bytes.data(), val->value.bytes.size());
    }

    for (auto& int32Value : val->value.int32Values) {
        protoVal->add_int32_values(int32Value);
    }

    for (auto& int64Value : val->value.int64Values) {
        protoVal->add_int64_values(int64Value);
    }

    for (auto& floatValue : val->value.floatValues) {
        protoVal->add_float_values(floatValue);
    }
}

void DefaultVehicleHal::rxMsg(void) {
    int  numBytes = 0;
    int32_t msgSize;
    do {
        // This is a variable length message.
        // Read the number of bytes to rx over the socket
        numBytes = read(mCurSocket, &msgSize, sizeof(msgSize));

        if (numBytes != sizeof(msgSize)) {
            // This happens when connection is closed
            ALOGD("%s: numBytes=%d, expected=4", __FUNCTION__, numBytes);
            break;
        }

        std::vector<uint8_t> msg = std::vector<uint8_t>(msgSize);

        numBytes = read(mCurSocket, msg.data(), msgSize);

        if ((numBytes == msgSize) && (msgSize > 0)) {
            // Received a message.
            parseRxProtoBuf(msg);
        } else {
            // This happens when connection is closed
            ALOGD("%s: numBytes=%d, msgSize=%d", __FUNCTION__, numBytes, msgSize);
            break;
        }
    } while (mExit == 0);
}

void DefaultVehicleHal::rxThread(void) {
    // Initialize the socket
    {
        int retVal;
        struct sockaddr_in servAddr;

        mSocket = socket(AF_INET, SOCK_STREAM, 0);
        if (mSocket < 0) {
            ALOGE("%s: socket() failed, mSocket=%d, errno=%d", __FUNCTION__, mSocket, errno);
            mSocket = -1;
            return;
        }

        bzero(&servAddr, sizeof(servAddr));
        servAddr.sin_family = AF_INET;
        servAddr.sin_addr.s_addr = INADDR_ANY;
        servAddr.sin_port = htons(DEBUG_SOCKET);

        retVal = bind(mSocket, reinterpret_cast<struct sockaddr*>(&servAddr), sizeof(servAddr));
        if(retVal < 0) {
            ALOGE("%s: Error on binding: retVal=%d, errno=%d", __FUNCTION__, retVal, errno);
            close(mSocket);
            mSocket = -1;
            return;
        }

        listen(mSocket, 1);

        // Set the socket to be non-blocking so we can poll it continouously
        fcntl(mSocket, F_SETFL, O_NONBLOCK);
    }

    while (mExit == 0) {
        struct sockaddr_in cliAddr;
        socklen_t cliLen = sizeof(cliAddr);
        int cSocket = accept(mSocket, reinterpret_cast<struct sockaddr*>(&cliAddr), &cliLen);

        if (cSocket >= 0) {
            {
                std::lock_guard<std::mutex> lock(mTxMutex);
                mCurSocket = cSocket;
            }
            ALOGD("%s: Incoming connection received on socket %d", __FUNCTION__, cSocket);
            rxMsg();
            ALOGD("%s: Connection terminated on socket %d", __FUNCTION__, cSocket);
            {
                std::lock_guard<std::mutex> lock(mTxMutex);
                mCurSocket = -1;
            }
        }

        // TODO:  Use a blocking socket?
        // Check every 100ms for a new socket connection
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Shutdown the socket
    close(mSocket);
    mSocket = -1;
}

// This function sets the default value of a property if we are interested in setting it.
// TODO:  Co-locate the default values with the configuration structure, to make it easier to
//          add new properties and their defaults.
void DefaultVehicleHal::setDefaultValue(VehiclePropValue* prop) {
    switch (prop->prop) {
    case toInt(VehicleProperty::INFO_MAKE):
        prop->value.stringValue = "Default Car";
        break;
    case toInt(VehicleProperty::HVAC_POWER_ON):
        prop->value.int32Values[0] = 1;
        break;
    case toInt(VehicleProperty::HVAC_DEFROSTER):
        prop->value.int32Values[0] = 0;
        break;
    case toInt(VehicleProperty::HVAC_RECIRC_ON):
        prop->value.int32Values[0] = 1;
        break;
    case toInt(VehicleProperty::HVAC_AC_ON):
        prop->value.int32Values[0] = 1;
        break;
    case toInt(VehicleProperty::HVAC_AUTO_ON):
        prop->value.int32Values[0] = 1;
        break;
    case toInt(VehicleProperty::HVAC_FAN_SPEED):
        prop->value.int32Values[0] = 3;
        break;
    case toInt(VehicleProperty::HVAC_FAN_DIRECTION):
        prop->value.int32Values[0] = toInt(VehicleHvacFanDirection::FACE);
        break;
    case toInt(VehicleProperty::HVAC_TEMPERATURE_SET):
        prop->value.floatValues[0] = 16;
        break;
    case toInt(VehicleProperty::NIGHT_MODE):
        prop->value.int32Values[0] = 0;
        break;
    case toInt(VehicleProperty::DRIVING_STATUS):
        prop->value.int32Values[0] = toInt(VehicleDrivingStatus::UNRESTRICTED);
        break;
    case toInt(VehicleProperty::GEAR_SELECTION):
        prop->value.int32Values[0] = toInt(VehicleGear::GEAR_PARK);
        break;
    case toInt(VehicleProperty::INFO_FUEL_CAPACITY):
        prop->value.floatValues[0] = 0.75f;
        break;
    case toInt(VehicleProperty::DISPLAY_BRIGHTNESS):
        prop->value.int32Values[0] = 7;
        break;
    case toInt(VehicleProperty::IGNITION_STATE):
        prop->value.int32Values[0] = toInt(VehicleIgnitionState::ON);
        break;
    case toInt(VehicleProperty::OBD2_LIVE_FRAME):
        // OBD2 is handled separately
        break;
    case toInt(VehicleProperty::OBD2_FREEZE_FRAME):
        // OBD2 is handled separately
        break;
    default:
        ALOGW("%s: propId=0x%x not found", __FUNCTION__, prop->prop);
        break;
    }
}

// Transmit a reply back to the emulator
void DefaultVehicleHal::txMsg(emulator::EmulatorMessage& txMsg) {
    std::string msgString;

    if (txMsg.SerializeToString(&msgString)) {
        int32_t msgLen = msgString.length();
        int retVal = 0;

        // TODO:  Prepend the message length to the string without a copy
        msgString.insert(0, reinterpret_cast<char*>(&msgLen), 4);

        // Send the message
        {
            std::lock_guard<std::mutex> lock(mTxMutex);
            if (mCurSocket != -1) {
                retVal = write(mCurSocket, msgString.data(), msgString.size());
            }
        }

        if (retVal < 0) {
            ALOGE("%s: Failed to tx message: retval=%d, errno=%d", __FUNCTION__, retVal, errno);
        }
    } else {
        ALOGE("%s: SerializeToString failed!", __FUNCTION__);
    }
}

// Updates the property value held in the HAL
StatusCode DefaultVehicleHal::updateProperty(const VehiclePropValue& propValue) {
    auto propId = propValue.prop;
    auto areaId = propValue.areaId;
    StatusCode status = StatusCode::INVALID_ARG;

    {
        std::lock_guard<std::mutex> lock(mPropsMutex);

        VehiclePropValue* internalPropValue = getVehiclePropValueLocked(propId, areaId);
        if (internalPropValue != nullptr) {
            internalPropValue->value = propValue.value;
            internalPropValue->timestamp = elapsedRealtimeNano();
            status = StatusCode::OK;
        }
    }
    return status;
}

VehicleHal::VehiclePropValuePtr DefaultVehicleHal::get(
        const VehiclePropValue& requestedPropValue, StatusCode* outStatus) {
    auto areaId = requestedPropValue.areaId;
    auto& pool = *getValuePool();
    auto propId = requestedPropValue.prop;
    StatusCode status;
    VehiclePropValuePtr v = nullptr;

    switch (propId) {
    case toInt(VehicleProperty::OBD2_LIVE_FRAME):
        v = pool.obtainComplex();
        status = fillObd2LiveFrame(&v);
        break;
    case toInt(VehicleProperty::OBD2_FREEZE_FRAME):
        v = pool.obtainComplex();
        status = fillObd2FreezeFrame(&v);
        break;
    default:
        {
            std::lock_guard<std::mutex> lock(mPropsMutex);

            VehiclePropValue *internalPropValue = getVehiclePropValueLocked(propId, areaId);
            if (internalPropValue != nullptr) {
                v = pool.obtain(*internalPropValue);
            }
        }

        if (v != nullptr) {
            status = StatusCode::OK;
        } else {
            status = StatusCode::INVALID_ARG;
        }
        break;
    }

    *outStatus = status;
    return v;
}

StatusCode DefaultVehicleHal::set(const VehiclePropValue& propValue) {
    StatusCode status = updateProperty(propValue);

    if (status == StatusCode::OK) {
        // Send property update to emulator
        emulator::EmulatorMessage msg;
        emulator::VehiclePropValue *val = msg.add_value();
        populateProtoVehiclePropValue(val, &propValue);
        msg.set_status(emulator::RESULT_OK);
        msg.set_msg_type(emulator::SET_PROPERTY_ASYNC);
        txMsg(msg);
    }

    return status;
}

// Parse supported properties list and generate vector of property values to hold current values.
void DefaultVehicleHal::onCreate() {
    // Initialize member variables
    mCurSocket = -1;
    mExit = 0;
    mSocket = -1;

    // Get the list of configurations supported by this HAL
    std::vector<VehiclePropConfig> configs = listProperties();

    for (auto& cfg : configs) {
        VehiclePropertyType propType = getPropType(cfg.prop);
        int32_t supportedAreas = cfg.supportedAreas;
        int32_t vecSize;

        // Set the vector size based on property type
        switch (propType) {
        case VehiclePropertyType::BOOLEAN:
        case VehiclePropertyType::INT32:
        case VehiclePropertyType::INT64:
        case VehiclePropertyType::FLOAT:
            vecSize = 1;
            break;
        case VehiclePropertyType::INT32_VEC:
        case VehiclePropertyType::FLOAT_VEC:
        case VehiclePropertyType::BYTES:
            // TODO:  Add proper support for these types
            vecSize = 1;
            break;
        case VehiclePropertyType::STRING:
            // Require individual handling
            vecSize = 0;
            break;
        case VehiclePropertyType::COMPLEX:
            switch (cfg.prop) {
            case toInt(VehicleProperty::OBD2_LIVE_FRAME):
                initObd2LiveFrame(cfg);
                break;
            default:
                // Need to handle each complex property separately
                break;
            }
            continue;
            break;
        case VehiclePropertyType::MASK:
        default:
            ALOGW("%s: propType=0x%x not found", __FUNCTION__, propType);
            vecSize = 0;
            break;
        }

        //  A global property will have supportedAreas = 0
        if (getPropArea(cfg.prop) == VehicleArea::GLOBAL) {
            supportedAreas = 0;
        }

        // This loop is a do-while so it executes at least once to handle global properties
        do {
            int32_t curArea = supportedAreas;

            // Clear the right-most bit of supportedAreas
            supportedAreas &= supportedAreas - 1;

            // Set curArea to the previously cleared bit
            curArea ^= supportedAreas;

            // Create a separate instance for each individual zone
            std::unique_ptr<VehiclePropValue> prop = createVehiclePropValue(propType, vecSize);
            prop->areaId = curArea;
            prop->prop = cfg.prop;
            setDefaultValue(prop.get());
            mProps.push_back(std::move(prop));
        } while (supportedAreas != 0);
    }

    // Start rx thread
    mThread = std::thread(&DefaultVehicleHal::rxThread, this);
}

StatusCode DefaultVehicleHal::fillObd2LiveFrame(VehiclePropValuePtr* v) {
    (*v)->value.int32Values = mObd2SensorStore->getIntegerSensors();
    (*v)->value.floatValues = mObd2SensorStore->getFloatSensors();
    (*v)->value.bytes = mObd2SensorStore->getSensorsBitmask();
    return StatusCode::OK;
}

StatusCode DefaultVehicleHal::fillObd2FreezeFrame(VehiclePropValuePtr* v) {
    (*v)->value.int32Values = mObd2SensorStore->getIntegerSensors();
    (*v)->value.floatValues = mObd2SensorStore->getFloatSensors();
    (*v)->value.bytes = mObd2SensorStore->getSensorsBitmask();
    (*v)->value.stringValue = "P0010";
    return StatusCode::OK;
}


}  // impl

}  // namespace V2_0
}  // namespace vehicle
}  // namespace automotive
}  // namespace hardware
}  // namespace android