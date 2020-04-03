/*
;    Project:       Open Vehicle Monitor System
;    Date:          5th July 2018
;
;    Changes:
;    1.0  Initial release
;
;    (C) 2011       Michael Stegen / Stegen Electronics
;    (C) 2011-2018  Mark Webb-Johnson
;    (C) 2011       Sonny Chen @ EPRO/DX

;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in
; all copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
; THE SOFTWARE.
*/

/*
;    Subproject:    Integration of support for the VW e-UP
;    Date:          01st April 2020
;
;    Changes:
;    0.1.0  Initial code
:           Code frame with correct TAG and vehicle/can registration
;
;    0.1.1  Started with some SOC capture demo code
;
;    0.1.2  Added VIN, speed, 12 volt battery detection
;
;    0.1.3  Added ODO (Dimitrie78), fixed speed
;
;    0.1.4  Added WLTP based ideal range, uncertain outdoor temperature
;
;    0.1.5  Finalized SOC calculation (sharkcow), added estimated range
;
;    0.1.6  Created a climate control first try, removed 12 volt battery status
;
;    0.1.7  Added status of doors
;
;    0.1.8  Added config page for the webfrontend. Features canwrite and modelyear.
;           Differentiation between model years is now possible.
;
;    0.1.9  "Fixed" crash on climate control. Added A/C indicator.
;           First shot on battery temperatur.
;
;    0.2.0  Added key detection and car_on / pollingstate routine
;
;    0.2.1  Removed battery temperature, corrected outdoor temperature
;
;    0.2.2  Collect VIN only once
;
;    0.2.3  Redesign climate control, removed bluetooth template
;
;    0.2.4  First implementation of the ringbus and ocu heartbeat
;
;    (C) 2020       Chris van der Meijden
;
;    Big thanx to sharkcow and Dimitrie78.
*/

#include "ovms_log.h"
static const char *TAG = "v-vweup";

#define VERSION "0.2.4"

#include <stdio.h>
#include "pcp.h"
#include "vehicle_vweup.h"
#include "metrics_standard.h"
#include "ovms_webserver.h"
#include "ovms_events.h"
#include "ovms_metrics.h"

static const OvmsVehicle::poll_pid_t vwup_polls[] =
{
  { 0, 0, 0x00, 0x00, { 0, 0, 0 } }
};

void sendOcuHeartbeat(TimerHandle_t timer)
  {
  OvmsVehicleVWeUP* vwup = (OvmsVehicleVWeUP*) pvTimerGetTimerID(timer);
  vwup->SendOcuHeartbeat();
  }

OvmsVehicleVWeUP::OvmsVehicleVWeUP()
  {
  ESP_LOGI(TAG, "Start VW e-Up vehicle module");
  memset (m_vin,0,sizeof(m_vin));

  RegisterCanBus(3,CAN_MODE_ACTIVE,CAN_SPEED_100KBPS);

  MyConfig.RegisterParam("vwup", "VW e-Up", true, true);
  ConfigChanged(NULL);
  PollSetPidList(m_can3, vwup_polls);
  PollSetState(0);
  vin_part1 = false;
  vin_part2 = false;
  vin_part3 = false;
  vwup_remote_climate_ticker = 0;
  ocu_working = false;
  ocu_what = false;

#ifdef CONFIG_OVMS_COMP_WEBSERVER
  WebInit();
#endif

  m_sendOcuHeartbeat = xTimerCreate("VW e-Up OCU heartbeat", 1000 / portTICK_PERIOD_MS, pdTRUE, this, sendOcuHeartbeat);
  }

OvmsVehicleVWeUP::~OvmsVehicleVWeUP()
  {
  ESP_LOGI(TAG, "Stop VW e-Up vehicle module");

#ifdef CONFIG_OVMS_COMP_WEBSERVER
  WebDeInit();
#endif
  }

bool OvmsVehicleVWeUP::SetFeature(int key, const char *value)
{
  switch (key)
  {
    case 15:
    {
      int bits = atoi(value);
      MyConfig.SetParamValueBool("vwup", "canwrite",  (bits& 1)!=0);
      return true;
    }
    default:
      return OvmsVehicle::SetFeature(key, value);
  }
}

const std::string OvmsVehicleVWeUP::GetFeature(int key)
{
  switch (key)
  {
    case 15:
    {
      int bits =
        ( MyConfig.GetParamValueBool("vwup", "canwrite",  false) ?  1 : 0);
      char buf[4];
      sprintf(buf, "%d", bits);
      return std::string(buf);
    }
    default:
      return OvmsVehicle::GetFeature(key);
  }
}

void OvmsVehicleVWeUP::ConfigChanged(OvmsConfigParam* param)
{
  ESP_LOGD(TAG, "VW e-Up reload configuration");
  
  vwup_enable_write  = MyConfig.GetParamValueBool("vwup", "canwrite", false);
  vwup_modelyear     = MyConfig.GetParamValueInt("vwup", "modelyear", DEFAULT_MODEL_YEAR);
}

void OvmsVehicleVWeUP::IncomingFrameCan3(CAN_frame_t* p_frame)
  {
  uint8_t *d = p_frame->data.u8;

  // This will log all incoming frames
  // ESP_LOGD(TAG, "IFC %03x 8 %02x %02x %02x %02x %02x %02x %02x %02x", p_frame->MsgID, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);

  switch (p_frame->MsgID) {

    case 0x61A: // SOC. Is this different for > 2019 models? 
      StandardMetrics.ms_v_bat_soc->SetValue(d[7]/2.0);
      if (vwup_modelyear >= 2020)
        {
          StandardMetrics.ms_v_bat_range_ideal->SetValue((260 * (d[7]/2.0)) / 100.0); // This is dirty. Based on WLTP only. Should be based on SOH.
        } else {
          StandardMetrics.ms_v_bat_range_ideal->SetValue((160 * (d[7]/2.0)) / 100.0); // This is dirty. Based on WLTP only. Should be based on SOH.
        }
      break;

    case 0x52D: // KM range left (estimated).
      if( d[0] != 0xFE )
        StandardMetrics.ms_v_bat_range_est->SetValue(d[0]);
      break;

    case 0x65F: // VIN
      switch (d[0]) {
          case 0x00:
            // Part 1
            m_vin[0] = d[5];
            m_vin[1] = d[6];
            m_vin[2] = d[7];
            vin_part1 = true;
            break;
          case 0x01:
            // Part 2
            if (vin_part1) {
              m_vin[3] = d[1];
              m_vin[4] = d[2];
              m_vin[5] = d[3];
              m_vin[6] = d[4];
              m_vin[7] = d[5];
              m_vin[8] = d[6];
              m_vin[9] = d[7];
              vin_part2 = true;
            }
            break;
          case 0x02:
            // Part 3 - VIN complete
            if (vin_part2 && !vin_part3) {
              m_vin[10] = d[1];
              m_vin[11] = d[2];
              m_vin[12] = d[3];
              m_vin[13] = d[4];
              m_vin[14] = d[5];
              m_vin[15] = d[6];
              m_vin[16] = d[7];
              m_vin[17] = 0;
              vin_part3 = true;
              StandardMetrics.ms_v_vin->SetValue((string) m_vin);
            }
            break;
      }
      break;

    case 0x65D: // ODO
      StandardMetrics.ms_v_pos_odometer->SetValue( ((uint32_t)(d[3] & 0xf) << 12) | ((UINT)d[2] << 8) | d[1] );
      break;

    case 0x320: // Speed
      // We need some awake message.
      if (StandardMetrics.ms_v_env_awake->IsStale())
      {
        StandardMetrics.ms_v_env_awake->SetValue(false);
      }
      StandardMetrics.ms_v_pos_speed->SetValue(((d[4] << 8) + d[3]-1)/190);
      break;

    case 0x527: // Outdoor temperature - untested. Wrong ID? If right, d[4] or d[5]?
      StandardMetrics.ms_v_env_temp->SetValue((d[5]/2)-50);
      break;

    case 0x3E3: // Cabin temperature
      // We should use:
      //
      // StandardMetrics.ms_v_env_cabintemp->SetValue((d[2]-100)/2);
      //
      // which is not implemented in the app.
      //
      // So instead we use as a quick workaround the PEM temperature:
      //
      StandardMetrics.ms_v_inv_temp->SetValue((d[2]-100)/2);
      break;

    case 0x470: // Doors
      StandardMetrics.ms_v_door_fl->SetValue((d[1] & 0x01) > 0);
      StandardMetrics.ms_v_door_fr->SetValue((d[1] & 0x02) > 0);
      break;

    // Check for running remote hvac.
    case 0x3E1:
        if (d[4] > 0) {
          StandardMetrics.ms_v_env_hvac->SetValue(true);
        } else {
          StandardMetrics.ms_v_env_hvac->SetValue(false);
        }
      break;

    case 0x575: // Key position
      switch  (d[0]) {
        case 0x00: // No key
          vehicle_vweup_car_on(false);
          break;
        case 0x01: // Key in position 1, no ignition
          vehicle_vweup_car_on(false);
          break;
        case 0x03: // Ignition is turned off
          break;
        case 0x05: // Ignition is turned on
          break;
        case 0x07: // Key in position 2, ignition on
          vehicle_vweup_car_on(true);
          break;
        case 0x0F: // Key in position 3, start the engine
          break;
      }
      break;

    case 0x400: // Welcome from ILM
    case 0x40C: // We know this one too. Climatronic.
    case 0x436: // Working in the ring. 
    case 0x439: // Who 436 and 439 and why do they differ on some cars?
      if (d[1] == 0x31 ) { // We should go to sleep
          ESP_LOGI(TAG, "Comfort CAN calls for sleep");
          xTimerStop(m_sendOcuHeartbeat, 0);
          break;
      }
      if (d[0] == 0x1D) { // We are called in the ring
          CAN_frame_t frame;
          memset(&frame, 0, sizeof(frame));

          frame.origin = m_can3;
          frame.FIR.U = 0;
          frame.FIR.B.DLC = 8;
          frame.FIR.B.FF = CAN_frame_std;
          frame.MsgID = 0x43D;
          frame.callback = NULL;
          frame.data.u8[0] = 0x00;
          if (ocu_working) {
            frame.data.u8[1] = 0x01;
          } else {
            frame.data.u8[1] = 0x11; // Ready to sleep. This is very important!
          }
          frame.data.u8[2] = 0x02;
          frame.data.u8[3] = 0x00;
          frame.data.u8[4] = 0x00;
          if (ocu_what) { // This is not understood yet
            frame.data.u8[5] = 0x14;
          } else {
            frame.data.u8[5] = 0x10;
          }
          frame.data.u8[6] = 0x00;
          frame.data.u8[7] = 0x00;
          if(vwup_enable_write) m_can3->Write(&frame); // We answer.
          break;
      }
      break;

    default:
      // This will log all unknown incoming frames
      // ESP_LOGD(TAG, "IFC %03x 8 %02x %02x %02x %02x %02x %02x %02x %02x", p_frame->MsgID, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
      break;
    }
  }

// Takes care of setting all the state appropriate when the car is on
// or off.
//
void OvmsVehicleVWeUP::vehicle_vweup_car_on(bool isOn)
  {
  if (isOn && !StandardMetrics.ms_v_env_on->AsBool())
    {
    // Log once that car is being turned on
    ESP_LOGI(TAG,"CAR IS ON");
    StandardMetrics.ms_v_env_on->SetValue(true);
    if (vwup_enable_write) PollSetState(1);
    }
  else if (!isOn && StandardMetrics.ms_v_env_on->AsBool())
    {
    // Log once that car is being turned off
    ESP_LOGI(TAG,"CAR IS OFF");
    StandardMetrics.ms_v_env_on->SetValue(false);
    if (vwup_enable_write) PollSetState(0);
    }
  }


// Wakeup implentation over the VW ring commands
// We need to register in the ring with a call to our self from 43D with 0x1D in the first byte

OvmsVehicle::vehicle_command_t OvmsVehicleVWeUP::CommandWakeup() {

  if(!vwup_enable_write)
    return Fail;
  
  ESP_LOGI(TAG, "Send Wakeup Command");
  
  CAN_frame_t frame;
  memset(&frame, 0, sizeof(frame));

  frame.origin = m_can3;
  frame.FIR.U = 0;
  frame.FIR.B.DLC = 8;
  frame.FIR.B.FF = CAN_frame_std;
  frame.MsgID = 0x43D;
  frame.callback = NULL;
  frame.data.u8[0] = 0x1D;
  frame.data.u8[1] = 0x02;
  frame.data.u8[2] = 0x02;
  frame.data.u8[3] = 0x00;
  frame.data.u8[4] = 0x00;
  frame.data.u8[5] = 0x14; // What does this?
  frame.data.u8[6] = 0x00;
  frame.data.u8[7] = 0x00;
  if (vwup_enable_write) m_can3->Write(&frame);

  ocu_working = true;

  vTaskDelay(100 / portTICK_PERIOD_MS);
  xTimerStart(m_sendOcuHeartbeat, 0);

  return Success;
}

////////////////////////////////////////////////////////////////////////
// Send a RemoteCommand on the CAN bus.
//
// Does nothing if @command is out of range


// Begin of remote climate control template
//
// This is just a template and does nothing!
// We are missing the CAN ID's yet to make it work

void OvmsVehicleVWeUP::SendCommand(RemoteCommand command)
  {

  unsigned char data[8];

  uint8_t length;
  length = 4;

  canbus *comfBus;
  comfBus = m_can3;

  bool new_data;
  new_data = false;

  switch (command)
    {
    // This is a placeholder. We need to find out the right procedure.
    case ENABLE_CLIMATE_CONTROL:
      if (vwup_remote_climate_ticker == 0) {
        vwup_remote_climate_ticker = 1800;
      } else {
        ESP_LOGI(TAG, "Enable Climate Control - already enabled");
        break;
      }
      ESP_LOGI(TAG, "Enable Climate Control");

      CommandWakeup();

      // 0x69E 29 58 00 01 climate on?
      data[0] = 0x29;
      data[1] = 0x58;
      data[2] = 0x00;
      data[3] = 0x01;
      new_data = true;
      break;
    case DISABLE_CLIMATE_CONTROL:
      if (vwup_remote_climate_ticker == 0) {
        ESP_LOGI(TAG, "Disable Climate Control - already disabled");
        break;
      } 
      vwup_remote_climate_ticker = 0;
      ESP_LOGI(TAG, "Disable Climate Control");
      // 0x69E 29 58 00 00 climate off?
      data[0] = 0x29;
      data[1] = 0x58;
      data[2] = 0x00;
      data[3] = 0x00;
      new_data = true;
      break;
    case AUTO_DISABLE_CLIMATE_CONTROL:
      vwup_remote_climate_ticker = 0;
      ESP_LOGI(TAG, "Auto Disable Climate Control");
      // 0x69E 29 58 00 00 climate off?
      data[0] = 0x29;
      data[1] = 0x58;
      data[2] = 0x00;
      data[3] = 0x00;
      new_data = true;
      break;
    default:
      return;
    }
    
    if (new_data) {
      // This does not work untill we find the right procedure for climate remote control
      // This also crashes OVMS if we are not connected to the comfort CAN
      comfBus->WriteStandard(0x69E, length, data);
      ocu_working = true;
      if (vwup_remote_climate_ticker == 0) ocu_working = false;
      ESP_LOGI(TAG, "Wrote Climate Control Message to Comfort CAN.");
    }
  }

OvmsVehicle::vehicle_command_t OvmsVehicleVWeUP::RemoteCommandHandler(RemoteCommand command)
  {
  ESP_LOGI(TAG, "RemoteCommandHandler");

  vwup_remote_command = command;
  SendCommand(vwup_remote_command);

  return Success;
  }

void OvmsVehicleVWeUP::SendOcuHeartbeat()
  {
  CAN_frame_t frame;
  memset(&frame, 0, sizeof(frame));

  frame.origin = m_can3;
  frame.FIR.U = 0;
  frame.FIR.B.DLC = 8;
  frame.FIR.B.FF = CAN_frame_std;
  frame.MsgID = 0x5A9;
  frame.callback = NULL;
  frame.data.u8[0] = 0x00;
  frame.data.u8[1] = 0x00;
  frame.data.u8[2] = 0x00;
  frame.data.u8[3] = 0x00;
  frame.data.u8[4] = 0x00;
  frame.data.u8[5] = 0x00;
  frame.data.u8[6] = 0x00;
  frame.data.u8[7] = 0x00;
  if (vwup_enable_write) m_can3->Write(&frame);
  }


OvmsVehicle::vehicle_command_t OvmsVehicleVWeUP::CommandHomelink(int button, int durationms)
  {
  ESP_LOGI(TAG, "CommandHomelink");
  return NotImplemented;
  }

OvmsVehicle::vehicle_command_t OvmsVehicleVWeUP::CommandClimateControl(bool climatecontrolon)
  {
  ESP_LOGI(TAG, "CommandClimateControl");
  if (vwup_enable_write)
    return RemoteCommandHandler(climatecontrolon ? ENABLE_CLIMATE_CONTROL : DISABLE_CLIMATE_CONTROL);
  else return NotImplemented;
  }

// End of remote climate contol template

void OvmsVehicleVWeUP::Ticker1(uint32_t ticker)
  {
  // This is just to be sure that we really have an asleep message. It has delay of 120 sec.
  if (StandardMetrics.ms_v_env_awake->IsStale())
    {
    StandardMetrics.ms_v_env_awake->SetValue(false);
    }

  // Autodisable climate control ticker (30 min.)
  if (vwup_remote_climate_ticker != 0) 
    {
    vwup_remote_climate_ticker--;
    if (vwup_remote_climate_ticker == 1) 
      {
        SendCommand(AUTO_DISABLE_CLIMATE_CONTROL);
      }
    }
  }

class OvmsVehicleVWeUPInit
  {
  public: OvmsVehicleVWeUPInit();
} OvmsVehicleVWeUPInit  __attribute__ ((init_priority (9000)));

OvmsVehicleVWeUPInit::OvmsVehicleVWeUPInit()
  {
  ESP_LOGI(TAG, "Registering Vehicle: VW e-Up (9000)");

  MyVehicleFactory.RegisterVehicle<OvmsVehicleVWeUP>("VWUP","VW e-Up");
  }
