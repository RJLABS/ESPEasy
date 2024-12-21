
#include "../Helpers/_CPlugin_Helper_mqtt.h"

#if FEATURE_MQTT
# include "../Commands/ExecuteCommand.h"

/***************************************************************************************
 * Parse MQTT topic for /cmd and /set ending to handle commands or TaskValueSet
 * Special C014 case: handleCmd = false and handleSet is true, so *only* pluginID 33 & 86 are accepted
 **************************************************************************************/
bool MQTT_handle_topic_commands(struct EventStruct *event,
                                bool                handleCmd,
                                bool                handleSet,
                                bool                tryRemoteConfig) {
  bool handled = false;

  // Topic  : event->String1
  // Message: event->String2
  String cmd;
  int    lastindex           = event->String1.lastIndexOf('/');
  const String lastPartTopic = event->String1.substring(lastindex + 1);

  if (!handled && handleCmd && equals(lastPartTopic, F("cmd"))) {
    // Example:
    // Topic: ESP_Easy/Bathroom_pir_env/cmd
    // Message: gpio,14,0
    // Full command:  gpio,14,0

    cmd = event->String2;

    // SP_C005a: string= ;cmd=gpio,12,0 ;taskIndex=12 ;string1=ESPT12/cmd ;string2=gpio,12,0
    handled = true;
  }

  if (!handled && handleSet && equals(lastPartTopic, F("set"))) {
    // Example:
    // Topic: ESP_Easy/DummyTask/DummyVar/set
    // Message: 14
    // Full command: TaskValueSet,DummyTask,DummyVar,14
    const String topic = event->String1.substring(0, lastindex);
    lastindex = topic.lastIndexOf('/');

    if (lastindex > -1) {
      String taskName        = topic.substring(0, lastindex);
      const String valueName = topic.substring(lastindex + 1);
      lastindex = taskName.lastIndexOf('/');

      if (lastindex > -1) {
        taskName = taskName.substring(lastindex + 1);

        const taskIndex_t    taskIndex    = findTaskIndexByName(taskName);
        const deviceIndex_t  deviceIndex  = getDeviceIndex_from_TaskIndex(taskIndex);
        const taskVarIndex_t taskVarIndex = event->Par2 - 1;
        uint8_t valueNr;

        if (validDeviceIndex(deviceIndex) && validTaskVarIndex(taskVarIndex)) {
          const int pluginID = Device[deviceIndex].Number;

          # ifdef USES_P033

          if ((pluginID == 33) || // Plugin 33 Dummy Device,
              // backward compatible behavior: if handleCmd = true then execute TaskValueSet regardless of AllowTaskValueSetAllPlugins
              ((handleCmd || Settings.AllowTaskValueSetAllPlugins()) && (pluginID != 86))) {
            // TaskValueSet,<task/device nr>,<value nr>,<value/formula (!ToDo) >, works only with new version of P033!
            valueNr = findDeviceValueIndexByName(valueName, taskIndex);

            if (validTaskVarIndex(valueNr)) { // value Name identified
              // Set a Dummy Device Value, device Number, var number and argument
              cmd     = strformat(F("TaskValueSet,%d,%d,%s"), taskIndex + 1, valueNr + 1, event->String2.c_str());
              handled = true;
            }
          }
          # endif // ifdef USES_P033
          # if defined(USES_P033) && defined(USES_P086)
          else
          # endif // if defined(USES_P033) && defined(USES_P086)
          # ifdef USES_P086

          if (pluginID == 86) { // Plugin 86 Homie receiver. Schedules the event defined in the plugin.
            // Does NOT store the value.
            // Use HomieValueSet to save the value. This will acknowledge back to the controller too.
            valueNr = findDeviceValueIndexByName(valueName, taskIndex);

            if (validTaskVarIndex(valueNr)) {
              cmd = strformat(F("event,%s="), valueName.c_str());

              if (Settings.TaskDevicePluginConfig[taskIndex][valueNr] == 3) {   // Quote String parameters. PLUGIN_086_VALUE_STRING
                cmd += wrapWithQuotes(event->String2);
              } else {
                if (Settings.TaskDevicePluginConfig[taskIndex][valueNr] == 4) { // Enumeration parameter, find Number of item.
                                                                                // PLUGIN_086_VALUE_ENUM
                  const String enumList = ExtraTaskSettings.TaskDeviceFormula[taskVarIndex];
                  int i                 = 1;
                  String part           = parseStringKeepCase(enumList, i);

                  while (!part.isEmpty()) {                      // lookup result in enum List, keep it backward compatible, but
                    if (part.equalsIgnoreCase(event->String2)) { // Homie spec says it should be case-sensitive...
                      break;
                    }
                    i++;
                    part = parseStringKeepCase(enumList, i);
                  }
                  cmd += i;
                  cmd += ',';
                }
                cmd += event->String2;
              }
              handled = true;
            }
          }
          # endif // ifdef USES_P086
        }
      }
    }
  }

  if (handled) {
    MQTT_execute_command(cmd, tryRemoteConfig);
  }
  return handled;
}

/*****************************************************************************************
 * Execute commands received via MQTT, sanitize event arguments with regard to commas vs =
 * event/asyncevent are added to queue, other commands executed immediately
 ****************************************************************************************/
void MQTT_execute_command(String& cmd,
                          bool    tryRemoteConfig) {
  // in case of event, store to buffer and return...
  const String command = parseString(cmd, 1);

  if (equals(command, F("event")) || equals(command, F("asyncevent"))) {
    if (Settings.UseRules) {
      // Need to sanitize the event a bit to allow for sending event values as MQTT messages.
      // For example:
      // Publish topic: espeasy_node/cmd_arg2/event/myevent/2
      // Message: 1
      // Actual event:  myevent=1,2

      // Strip out the "event" or "asyncevent" part, leaving the actual event string
      String args = parseStringToEndKeepCase(cmd, 2);

      {
        // Get the first part upto a parameter separator
        // Example: "myEvent,1,2,3", which needs to be converted to "myEvent=1,2,3"
        // N.B. This may contain the first eventvalue too
        // e.g. "myEvent=1,2,3" => "myEvent=1"
        String eventName    = parseStringKeepCase(args, 1);
        String eventValues  = parseStringToEndKeepCase(args, 2);
        const int equal_pos = eventName.indexOf('=');

        if (equal_pos != -1) {
          // We found an '=' character, so the actual event name is everything before that char.
          eventName   = args.substring(0, equal_pos);
          eventValues = args.substring(equal_pos + 1); // Rest of the event, after the '=' char
        }

        if (eventValues.startsWith(F(","))) {
          // Need to reconstruct the event to get rid of calls like these:
          // myevent=,1,2
          eventValues = eventValues.substring(1);
        }

        // Now reconstruct the complete event
        // Without event values: "myEvent" (no '=' char)
        // With event values: "myEvent=1,2,3"

        // Re-using the 'cmd' String as that has pre-allocated memory which is
        // known to be large enough to hold the entire event.
        args = eventName;

        if (eventValues.length() > 0) {
          // Only append an = if there are eventvalues.
          args += '=';
          args += eventValues;
        }
      }

      // Check for duplicates, as sometimes a node may have multiple subscriptions to the same topic.
      // Then it may add several of the same events in a burst.
      eventQueue.addMove(std::move(args), true);
    }
  } else {
    ExecuteCommand(INVALID_TASK_INDEX, EventValueSource::Enum::VALUE_SOURCE_MQTT, cmd.c_str(), true, true, tryRemoteConfig);
  }
}

bool MQTT_protocol_send(EventStruct *event,
                        String       pubname,
                        bool         retainFlag) {
  bool success                = false;
  const bool contains_valname = pubname.indexOf(F("%valname%")) != -1;

  const uint8_t valueCount = getValueCountForTask(event->TaskIndex);

  for (uint8_t x = 0; x < valueCount; ++x) {
    // MFD: skip publishing for values with empty labels (removes unnecessary publishing of unwanted values)
    if (getTaskValueName(event->TaskIndex, x).isEmpty()) {
      continue; // we skip values with empty labels
    }
    String tmppubname = pubname;

    if (contains_valname) {
      parseSingleControllerVariable(tmppubname, event, x, false);
    }
    parseControllerVariables(tmppubname, event, false);
    String value;

    if (event->sensorType == Sensor_VType::SENSOR_TYPE_STRING) {
      value = event->String2.substring(0, 20); // For the log
    } else {
      value = formatUserVarNoCheck(event, x);
    }
    # ifndef BUILD_NO_DEBUG

    if (loglevelActiveFor(LOG_LEVEL_DEBUG)) {
      addLog(LOG_LEVEL_DEBUG, strformat(F("MQTT C%03d : %s %s"), event->ControllerIndex, tmppubname.c_str(), value.c_str()));
    }
    # endif // ifndef BUILD_NO_DEBUG

    // Small optimization so we don't try to copy potentially large strings
    if (event->sensorType == Sensor_VType::SENSOR_TYPE_STRING) {
      if (MQTTpublish(event->ControllerIndex, event->TaskIndex, tmppubname.c_str(), event->String2.c_str(),
                      retainFlag)) {
        success = true;
      }
    } else {
      // Publish using move operator, thus tmppubname and value are empty after this call
      if (MQTTpublish(event->ControllerIndex, event->TaskIndex, std::move(tmppubname), std::move(value),
                      retainFlag)) {
        success = true;
      }
    }
  }
  return success;
}

# if FEATURE_MQTT_DISCOVER

bool MQTT_SendAutoDiscovery(controllerIndex_t ControllerIndex, cpluginID_t CPluginID) {
  bool success = true;

  MakeControllerSettings(ControllerSettings); // -V522

  if (!AllocatedControllerSettings()) {
    return false;
  }
  LoadControllerSettings(ControllerIndex, *ControllerSettings);

  if (ControllerSettings->mqtt_autoDiscovery()

      // && (ControllerSettings->MqttAutoDiscoveryTrigger[0] != 0)
      && (ControllerSettings->MqttAutoDiscoveryTopic[0] != 0)) {
    // Dispatch autoDiscovery per supported method
    switch (CPluginID) {
      case 5: // CPLUGIN_ID_005
        success = MQTT_HomeAssistant_SendAutoDiscovery(ControllerIndex, *ControllerSettings);
        break;
    }

    //
  }

  return success;
}

/********************************************************
 * Send MQTT AutoDiscovery in Home Assistant format
 *******************************************************/
bool MQTT_HomeAssistant_SendAutoDiscovery(controllerIndex_t         ControllerIndex,
                                          ControllerSettingsStruct& ControllerSettings) {
  String discoveryMessage;

  discoveryMessage.reserve(128);

  // TODO Send global info

  // TODO Send plugin info
  for (taskIndex_t x = 0; x < TASKS_MAX; ++x) {
    const pluginID_t pluginID = Settings.getPluginID_for_task(x);

    if (validPluginID_fullcheck(pluginID)) {
      LoadTaskSettings(x);
      const deviceIndex_t DeviceIndex = getDeviceIndex_from_TaskIndex(x);


      // Device is enabled so send information
      if (validDeviceIndex(DeviceIndex) &&
          Settings.TaskDeviceEnabled[x] &&                // task enabled?
          Settings.TaskDeviceSendData[ControllerIndex][x] // selected for this controller?
          ) {
        const String deviceName = getTaskDeviceName(x);
        std::vector<DiscoveryItem> discoveryItems;
        int valueCount = getValueCountForTask(x);

        // Translate Device[].VType to usable VType per value for MQTT AutoDiscovery
        if (MQTT_DiscoveryGetDeviceVType(DeviceIndex, x, discoveryItems, valueCount)) {
          for (size_t s = 0; s < discoveryItems.size(); ++s) {
            String tmp;
            String base_topic(ControllerSettings.MqttAutoDiscoveryTopic);
            String valuename;
            struct EventStruct TempEvent(x); // FIXME Check if this has enough data

            switch (discoveryItems[s].VType) {
              // VType values to support, mapped to device classes:
              case Sensor_VType::SENSOR_TYPE_SWITCH:

                for (uint8_t v = 0; v < discoveryItems[s].valueCount; ++v) {
                  String tmppubname = base_topic;
                  parseSingleControllerVariable(tmppubname, &TempEvent, v, false);
                  parseDeviceClassVariable(tmppubname, F("switch"), false);
                  parseControllerVariables(tmppubname, &TempEvent, false); // Replace this last
                  tmp += strformat(F("{\"~\":\"%s\",\"name\":%s,\"cmd_t\":\"~/set\",\"stat_t\":\"~/%s\"},"),
                                   tmppubname.c_str(), deviceName.c_str(), getTaskValueName(x, v).c_str());
                }
                discoveryMessage += tmp;
                break;
              case Sensor_VType::SENSOR_TYPE_TEMP_HUM:
              case Sensor_VType::SENSOR_TYPE_TEMP_BARO:
              case Sensor_VType::SENSOR_TYPE_TEMP_HUM_BARO:
              case Sensor_VType::SENSOR_TYPE_TEMP_EMPTY_BARO:
              case Sensor_VType::SENSOR_TYPE_WIND:
              case Sensor_VType::SENSOR_TYPE_ANALOG_ONLY:
              case Sensor_VType::SENSOR_TYPE_TEMP_ONLY:
              case Sensor_VType::SENSOR_TYPE_HUM_ONLY:
              case Sensor_VType::SENSOR_TYPE_LUX_ONLY:
              case Sensor_VType::SENSOR_TYPE_DISTANCE_ONLY:
              case Sensor_VType::SENSOR_TYPE_DIRECTION_ONLY:
              case Sensor_VType::SENSOR_TYPE_DUSTPM2_5_ONLY:
              case Sensor_VType::SENSOR_TYPE_DUSTPM1_0_ONLY:
              case Sensor_VType::SENSOR_TYPE_DUSTPM10_ONLY:
              case Sensor_VType::SENSOR_TYPE_MOISTURE_ONLY:
              case Sensor_VType::SENSOR_TYPE_CO2_ONLY:
              case Sensor_VType::SENSOR_TYPE_GPS_ONLY:
              case Sensor_VType::SENSOR_TYPE_UV_ONLY:
              case Sensor_VType::SENSOR_TYPE_UV_INDEX_ONLY:
              case Sensor_VType::SENSOR_TYPE_IR_ONLY:
                break;

              // VType values to ignore
              case Sensor_VType::SENSOR_TYPE_NONE:
              case Sensor_VType::SENSOR_TYPE_SINGLE:
              case Sensor_VType::SENSOR_TYPE_DUAL:
              case Sensor_VType::SENSOR_TYPE_TRIPLE:
              case Sensor_VType::SENSOR_TYPE_QUAD:
              case Sensor_VType::SENSOR_TYPE_DIMMER:
              case Sensor_VType::SENSOR_TYPE_STRING:
              case Sensor_VType::SENSOR_TYPE_ULONG:
              #  if FEATURE_EXTENDED_TASK_VALUE_TYPES
              case Sensor_VType::SENSOR_TYPE_UINT32_DUAL:
              case Sensor_VType::SENSOR_TYPE_UINT32_TRIPLE:
              case Sensor_VType::SENSOR_TYPE_UINT32_QUAD:
              case Sensor_VType::SENSOR_TYPE_INT32_SINGLE:
              case Sensor_VType::SENSOR_TYPE_INT32_DUAL:
              case Sensor_VType::SENSOR_TYPE_INT32_TRIPLE:
              case Sensor_VType::SENSOR_TYPE_INT32_QUAD:
              case Sensor_VType::SENSOR_TYPE_UINT64_SINGLE:
              case Sensor_VType::SENSOR_TYPE_UINT64_DUAL:
              case Sensor_VType::SENSOR_TYPE_INT64_SINGLE:
              case Sensor_VType::SENSOR_TYPE_INT64_DUAL:
              case Sensor_VType::SENSOR_TYPE_DOUBLE_SINGLE:
              case Sensor_VType::SENSOR_TYPE_DOUBLE_DUAL:
              #  endif // if FEATURE_EXTENDED_TASK_VALUE_TYPES
              case Sensor_VType::SENSOR_TYPE_NOT_SET:
                break;
            }
          }
        }
      }
    }
  }

  // TODO Send discovery message
  return false;
}

bool MQTT_DiscoveryGetDeviceVType(deviceIndex_t             DeviceIndex,
                                  taskIndex_t               TaskIndex,
                                  std::vector<DiscoveryItem>discoveryItems,
                                  int                       valueCount) {
  size_t orgLen      = discoveryItems.size();
  Sensor_VType VType = Device[DeviceIndex].VType;

  // For those plugin IDs that don't have an explicitly set VType, or the wrong VType set
  switch (Device[DeviceIndex].Number) {
    case 2:   // Analog input
    case 7:   // PCF8591
    case 25:  // ADS1x15
    case 60:  // MCP3221
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_ANALOG_ONLY, valueCount, 0));
      break;
    case 4:   // Dallas temperature sensors
    case 24:  // MLX90614
    case 39:  // SPI Thermosensors
    case 69:  // LM75A
    case 150: // TMP117
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_TEMP_ONLY, valueCount, 0));
      break;
    case 10:  // BH1750
    case 168: // VEML6030/VEML7700
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_LUX_ONLY, valueCount, 0));
      break;
    case 13:  // HCSR04
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_DISTANCE_ONLY, 1, 0));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_SWITCH, 1, 1));
      break;
    case 14: // SI70xx
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_TEMP_HUM, 1, 0));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_ANALOG_ONLY, 1, 2));
      break;
    case 15: // TSL2561 Only Lux / IR / Broadband values made available
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_LUX_ONLY, 1, 0));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_IR_ONLY, 1, 1));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_LUX_ONLY, 1, 2));
      break;
    case 18:  // GP2Y10
    case 144: // Vindriktning / PM1006K
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_DUSTPM2_5_ONLY, valueCount, 0));
      break;
    case 47:  // Soil Moisture
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_TEMP_ONLY, 1, 0));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_MOISTURE_ONLY, 1, 1));

      if (valueCount == 3) {
        discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_LUX_ONLY, 1, 2));
      }
      break;
    case 49: // MHZ19
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_CO2_ONLY, 1, 0));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_TEMP_ONLY, 1, 1));
      break;
    case 53:  // PMSx003
    case 175: // PMSx003i (I2C)
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_DUSTPM1_0_ONLY, 1, 0));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_DUSTPM2_5_ONLY, 1, 1));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_DUSTPM10_ONLY, 1, 2));
      break;
    case 56: // SDS011
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_DUSTPM2_5_ONLY, 1, 0));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_DUSTPM10_ONLY, 1, 1));
      break;
    case 74: // TSL2591
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_LUX_ONLY, 3, 0));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_UV_ONLY, 1, 3));
      break;
    case 84: // VEML6070 (UV and Index only)
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_UV_ONLY, 1, 0));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_UV_INDEX_ONLY, 1, 1));
      break;
    case 106: // BME68x
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_TEMP_HUM_BARO, valueCount, 0));
      break;
    case 107: // SI1145
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_LUX_ONLY, 1, 0));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_IR_ONLY, 1, 1));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_UV_ONLY, 1, 2));
      break;
    case 110: // VL53L0X
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_DISTANCE_ONLY, 1, 0));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_DIRECTION_ONLY, 1, 1));
      break;
    case 113: // VL53L1X
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_DISTANCE_ONLY, 1, 0));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_LUX_ONLY, 1, 1));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_DIRECTION_ONLY, 1, 2));
      break;
    case 114: // VEML6075
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_UV_ONLY, 2, 0));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_UV_INDEX_ONLY, 1, 2));
      break;
    case 127: // CDM7160
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_CO2_ONLY, valueCount, 0));
      break;
    case 133: // LTR390
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_UV_ONLY, 1, 0));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_UV_INDEX_ONLY, 1, 1));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_LUX_ONLY, 2, 2));
      break;
    case 134: // A02YYUW
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_DISTANCE_ONLY, valueCount, 0));
      break;
    case 135: // SCD4x
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_CO2_ONLY, 1, 0));
      discoveryItems.push_back(DiscoveryItem(Sensor_VType::SENSOR_TYPE_TEMP_HUM, 1, 1));
      break;

    // Deliberately ignored/skipped for now:
    case 3:  // Pulse: Needs special handling/not usable?
    case 26: // Sysinfo: Needs special handling/not usable?
    case 33: // Dummy: Needs special handling/not usable?
    case 61: // Keypad: Not a switch
    case 62: // Touch Keypad: Not a switch
    case 63: // Touch Keypad: Not a switch
      // Reset
      VType = Sensor_VType::SENSOR_TYPE_NONE;
      break;

    // To be reviewed/considered later:
    case 8:   // RFID Wiegand
    case 17:  // RFID PN532
    case 27:  // INA219
    case 40:  // RFID ID12
    case 45:  // MPU6050
    case 50:  // TCS34725 RGB: Needs special handling
    case 52:  // SensAir: Needs special handling
    case 64:  // APDS9960: Needs special handling
    case 66:  // VEML6040 RGB: Needs special handling
    case 67:  // HX711 Load cell
    case 71:  // Kamstrup Heat
    case 76:  // HLW8012
    case 77:  // CSE7766
    case 78:  // Eastron: Needs special handling
    case 82:  // GPS: Needs special handling
    case 83:  // SGP30 TVOC/eCO2
    case 85:  // AcuDC243
    case 90:  // CCS811 TVOC/eCO2
    case 92:  // DL-bus
    case 93:  // Mitsubishi Heatpump
    case 102: // PZEM004
    case 103: // Atlas EZO
    case 108: // DDS238
    case 111: // RFID RC522
    case 112: // AS7265x: Needs special handling
    case 115: // MAX1704x
    case 117: // SCD30 CO2: Needs special handling
    case 132: // INA3221
    case 142: // AS5600 Angle/rotation
    case 145: // MQxxx gases
    case 147: // SGP4x
    case 151: // Honeywell pressure
    case 159: // LD2410
    case 163: // RadSens
    case 164: // ENS160
    case 167: // Vindstyrka: Needs special handling
    case 169: // AS3935 Lightning
      // Reset
      VType = Sensor_VType::SENSOR_TYPE_NONE;
      break;
  }

  // Use Device VType setting if not overridden or reset
  if ((Sensor_VType::SENSOR_TYPE_NONE != VType) && (discoveryItems.size() == 0)) {
    discoveryItems.push_back(DiscoveryItem(VType, valueCount, 0));
  }
  struct EventStruct TempEvent(TaskIndex);
  String dummy;

  // Get value VTypes from plugin
  if (PluginCall(PLUGIN_GET_DISCOVERY_VTYPES, &TempEvent, dummy)) {
    uint8_t maxVar = VARS_PER_TASK;
    discoveryItems.clear();        // Plugin can override, clear any defaults acquired earlier

    for (; maxVar > 0; --maxVar) { // Only include minimal used values
      if (TempEvent.ParN[maxVar - 1] != 0) {
        break;
      }
    }

    for (uint8_t v = 0; v < maxVar; ++v) {
      discoveryItems.push_back(DiscoveryItem(static_cast<Sensor_VType>(TempEvent.ParN[v]), 1, v));
    }
  }

  return orgLen != discoveryItems.size(); // Something added?
}

# endif // if FEATURE_MQTT_DISCOVER

#endif // if FEATURE_MQTT
