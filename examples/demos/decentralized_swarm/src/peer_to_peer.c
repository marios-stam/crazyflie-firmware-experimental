/**
 * ,---------,       ____  _ __
 * |  ,-^-,  |      / __ )(_) /_______________ _____  ___
 * | (  O  ) |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * | / ,--´  |    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *    +------`   /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * Crazyflie control firmware
 *
 * Copyright (C) 2019 Bitcraze AB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, in version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * peer_to_peer.c - App layer application of simple demonstartion peer to peer
 *  communication. Two crazyflies need this program in order to send and receive.
 */


#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "app.h"

#include "FreeRTOS.h"
#include "task.h"

#include "radiolink.h"
#include "configblock.h"

#include "log.h"
#include "float.h"
#include <math.h>
#include <stdlib.h>
#include "estimator_kalman.h"
#include "ledseq.h"
#include "timers.h"
// #include "math3d.h" //TODO: import all functions and structs from math3d.h instead of mine
#include "positions.h"
#include "param.h"
#include "crtp_commander_high_level.h"
#include "sensors.h"
#include "pm.h"
#include "supervisor.h"
#include "peer_localization.h"


#define DEBUG_MODULE "P2P"
#include "debug.h"

#define TAKE_OFF_HEIGHT 0.6f

#define BROADCAST_FEQUENCY_HZ 15
#define BROADCAST_PERIOD_MS (1000 / BROADCAST_FEQUENCY_HZ)

#define CALC_NEXT_FEQUENCY_HZ 3
#define CALC_NEXT_PERIOD_MS (1000 / CALC_NEXT_FEQUENCY_HZ)

#define INTER_DIST 0.8f //distance between crazyflies
#define MAX_ADDRESS 10 //all copter addresses must be between 0 and max(MAX_ADDRESS,9)
#define LED_ESTIMATOR_STUCK        LED_GREEN_R
#define LED_CRASH                  LED_GREEN_R

// position lock settings
#define LOCK_LENGTH 50
#define LOCK_THRESHOLD 0.001f
#define POSITION_LOCK_TIMEOUT 10000

// NEXT DELTA
#define MAXIMUM_NEXT_DELTA 0.2f
#define DELTA_DURATION 3.0f //sec duration to go to next delta

#define HOVERING_TIME 8000 //ms
#define POSITION_UPDATE_TIMEOUT_MS 1500 //ms Timeout to ignore position updates from another copter

//Landing to charging pad
#define MAX_PAD_ERR 0.005
#define LANDING_HEIGHT 0.12f
#define GO_TO_PAD_DURATION 2.0f //sec duration to go to charging pad

// BOUNDS DEFINITIONS
#define MIN_X_BOUND -2.5f
#define MAX_X_BOUND  1.0f

#define MIN_Y_BOUND -2.0f
#define MAX_Y_BOUND  2.0f

#define MIN_Z_BOUND -0.4f
#define MAX_Z_BOUND  1.5f

static xTimerHandle sendPosTimer;
static xTimerHandle stateTransitionTimer;

static bool isInit = false;

// States of the state machine 
enum State {
    // Initialization
    STATE_IDLE = 0,
    STATE_WAIT_FOR_POSITION_LOCK,

    STATE_WAIT_FOR_TAKE_OFF, // Charging
    STATE_TAKING_OFF,
    STATE_HOVERING,
    STATE_GOING_TO_DELTA_POINT,
    STATE_GOING_TO_PAD,
    STATE_WAITING_AT_PAD,
    STATE_LANDING,
    STATE_CHECK_CHARGING,
    STATE_REPOSITION_ON_PAD,

    STATE_CRASHED,
};

static enum State state = STATE_IDLE;

static P2PPacket p_reply;

//Landing to pad
static float stabilizeEndTime;
static float landingTimeCheckCharge;


// Log and param ids
static logVarId_t logIdStateEstimateX;
static logVarId_t logIdStateEstimateY;
static logVarId_t logIdStateEstimateZ;
static logVarId_t logIdKalmanVarPX;
static logVarId_t logIdKalmanVarPY;
static logVarId_t logIdKalmanVarPZ;
static logVarId_t logIdPmState;
static logVarId_t logIdlighthouseEstBs0Rt;
static logVarId_t logIdlighthouseEstBs1Rt;
static paramVarId_t paramIdStabilizerController;
static paramVarId_t paramIdCommanderEnHighLevel;
static paramVarId_t paramIdLighthouseMethod;
static paramVarId_t paramIdCollisionAvoidance;

static uint8_t my_id;


static Position my_pos;
static float previous[3];
static float padX = 0.0;
static float padY = 0.0;
static float padZ = 0.0;

static uint32_t now = 0;
static uint32_t hovering_start_time = 0;
static uint32_t position_lock_start_time = 0;

// LEDs Interface
static bool seq_estim_stuck_running = false;
static uint8_t seq_crash_running = 0;

ledseqStep_t seq_flashing_def[] = {
  { true, LEDSEQ_WAITMS(50)},
  {false, LEDSEQ_WAITMS(50)},
  { true, LEDSEQ_WAITMS(50)},
  {false, LEDSEQ_WAITMS(50)},
  { true, LEDSEQ_WAITMS(50)},
  {false, LEDSEQ_WAITMS(50)},
  { true, LEDSEQ_WAITMS(50)},
  {false, LEDSEQ_WAITMS(50)},
  { true, LEDSEQ_WAITMS(50)},
  {false, LEDSEQ_WAITMS(50)},
  { true, LEDSEQ_WAITMS(50)},
  {false, LEDSEQ_WAITMS(50)},
  { true, LEDSEQ_WAITMS(50)},
  {false, LEDSEQ_WAITMS(300)},

  {    0, LEDSEQ_LOOP},
  
};

ledseqContext_t seq_estim_stuck = {
  .sequence = seq_flashing_def,
  .led = LED_ESTIMATOR_STUCK,
};

ledseqContext_t seq_crash = {
  .sequence = seq_flashing_def,
  .led = LED_CRASH,
};


static float lockData[LOCK_LENGTH][3];
static bool takeOffWhenReady = false;
static bool terminateTrajectoryAndLand = false;
static uint32_t lockWriteIndex;

Position offset = {0, 0, 0};

// getting  parameters
static float getX() { return (float) logGetFloat(logIdStateEstimateX); }
static float getY() { return (float) logGetFloat(logIdStateEstimateY); }
static float getZ() { return (float) logGetFloat(logIdStateEstimateZ); }
static float getVarPX() { return logGetFloat(logIdKalmanVarPX); }
static float getVarPY() { return logGetFloat(logIdKalmanVarPY); }
static float getVarPZ() { return logGetFloat(logIdKalmanVarPZ); }
static bool isBatLow() { return logGetInt(logIdPmState) == lowPower; }
static bool isCharging() { return logGetInt(logIdPmState) == charging; }
static bool isLighthouseAvailable() { return logGetFloat(logIdlighthouseEstBs0Rt) >= 0.0f || logGetFloat(logIdlighthouseEstBs1Rt) >= 0.0f; }
static void enableHighlevelCommander() { paramSetInt(paramIdCommanderEnHighLevel, 1); }
static void enableCollisionAvoidance() { paramSetInt(paramIdCollisionAvoidance, 1); }

static void resetLockData() {
    lockWriteIndex = 0;
    for (uint32_t i = 0; i < LOCK_LENGTH; i++) {
      lockData[i][0] = FLT_MAX;
      lockData[i][1] = FLT_MAX;
      lockData[i][2] = FLT_MAX;
    }
}

static bool hasLock() {
  bool result = false;

  // Store current state
  lockData[lockWriteIndex][0] = getVarPX();
  lockData[lockWriteIndex][1] = getVarPY();
  lockData[lockWriteIndex][2] = getVarPZ();

  lockWriteIndex++;
  if (lockWriteIndex >= LOCK_LENGTH) {
    lockWriteIndex = 0;
  }

  // Check if we have a lock
  int count = 0;

  float lXMax = FLT_MIN;
  float lYMax = FLT_MIN;
  float lZMax = FLT_MIN;

  float lXMin = FLT_MAX;
  float lYMin = FLT_MAX;
  float lZMin = FLT_MAX;

  for (int i = 0; i < LOCK_LENGTH; i++) {
    if (lockData[i][0] != FLT_MAX) {
      count++;

      lXMax = fmaxf(lXMax, lockData[i][0]);
      lYMax = fmaxf(lYMax, lockData[i][1]);
      lZMax = fmaxf(lZMax, lockData[i][2]);

      lXMin = fminf(lXMax, lockData[i][0]);
      lYMin = fminf(lYMin, lockData[i][1]);
      lZMin = fminf(lZMin, lockData[i][2]);
    }
  }

  result =
    (count >= LOCK_LENGTH) &&
    ((lXMax - lXMin) < LOCK_THRESHOLD) &&
    ((lYMax - lYMin) < LOCK_THRESHOLD) &&
    ((lZMax - lZMin) < LOCK_THRESHOLD &&
    isLighthouseAvailable() &&  // Make sure we have a deck and the Lighthouses are powered
    sensorsAreCalibrated());

  return result;
}


void p2pcallbackHandler(P2PPacket *p)
{
    // Parse the data from the other crazyflie and print it
    // uint8_t rssi = p->rssi;
    uint8_t received_id = p->data[0];
    // uint8_t counter = p->data[1];

    positionMeasurement_t pos_measurement;
    memcpy(&pos_measurement.pos, &(p->data[2]), sizeof(Position));
    
    // DEBUG_PRINT("===================================================\n");
    // DEBUG_PRINT("[RSSI: -%d dBm] Message from CF nr. %d  with counter: %d --> (%.2f , %.2f , %.2f)\n", rssi, received_id, counter,(double)pos_received.x,(double)pos_received.y,(double)pos_received.z);    
    
    pos_measurement.source =  MeasurementSourceLighthouse;
    pos_measurement.stdDev = 0.01f; //
    peerLocalizationTellPosition(received_id,&pos_measurement);//TODO: if id is 0--> PROBLEM WITH THE LOGIC OF THE PEER LOCALIZATION (maybe add 1 to the id)
}

static void initPacket(){
    p_reply.port=0x00;
    // Get the current address of the crazyflie and obtain
    //   the last two digits and send it as the first byte
    //   of the payload
    uint64_t address = configblockGetRadioAddress();
    my_id =(uint8_t)((address) & 0x00000000ff);
    p_reply.data[0]=my_id;
}

static void initLogIds(){
    logIdStateEstimateX = logGetVarId("stateEstimate", "x");
    logIdStateEstimateY = logGetVarId("stateEstimate", "y");
    logIdStateEstimateZ = logGetVarId("stateEstimate", "z");
}

static bool outOfBounds() {
    return my_pos.x > MAX_X_BOUND || my_pos.x < MIN_X_BOUND 
        || my_pos.y > MAX_Y_BOUND || my_pos.y < MIN_Y_BOUND 
        || my_pos.z > MAX_Z_BOUND || my_pos.z < MIN_Z_BOUND;
}

// timers
static void sendPosition(xTimerHandle timer) {

    if (state <= STATE_WAIT_FOR_TAKE_OFF || state >= STATE_CRASHED )
        return;
        
    static uint8_t counter=0;
    initPacket();

    my_pos.x=getX();
    my_pos.y=getY();
    my_pos.z=getZ();
    memcpy(&p_reply.data[2], &my_pos, sizeof(Position));
    
    if (previous[0]==my_pos.x && previous[1]==my_pos.y && previous[2]==my_pos.z) {
        // DEBUG_PRINT("Same value detected\n");
        if (!seq_estim_stuck_running){
            ledseqRun(&seq_estim_stuck);
            seq_estim_stuck_running=1;
        }

        logResetAll();//TODO: it seems to fix the problem, but I'm not sure about it
        initLogIds();
        
    }else{
        if (seq_estim_stuck_running)            
            ledseqStop(&seq_estim_stuck);
        seq_estim_stuck_running=0;
    }
    
    previous[0]=my_pos.x;
    previous[1]=my_pos.y;
    previous[2]=my_pos.z;
    
    p_reply.data[1] = counter++;
    
    //get current position and send it as the payload
    // DEBUG_PRINT("MY POSITION: "); PRINT_POSITION_3D(my_pos);
    p_reply.size=sizeof(Position)+2;//+2 for the id and counter
    radiolinkSendP2PPacketBroadcast(&p_reply);
}

static void stateTransition(xTimerHandle timer){
    // In the following checks , sequence of checks is important 
    
    if(supervisorIsTumbled()) {
        state = STATE_CRASHED;
    }
    else if (outOfBounds()) {
        DEBUG_PRINT("Out of bounds, stopping\n");
        crtpCommanderHighLevelLand(padZ, 1.0);
        state = STATE_LANDING;
    }
    else if (isBatLow()) {
        DEBUG_PRINT("Battery low, stopping\n");
        terminateTrajectoryAndLand=1;
    }

    now = xTaskGetTickCount();
    uint32_t dt;
    switch(state) {
        case STATE_IDLE:
        DEBUG_PRINT("Let's go! Waiting for position lock...\n");
        //reseting
        resetLockData();
        if (seq_crash_running == 1){
            ledseqStop(&seq_crash);
            seq_crash_running=0;
        }
        
        position_lock_start_time = now;
        state = STATE_WAIT_FOR_POSITION_LOCK;
        break;
        case STATE_WAIT_FOR_POSITION_LOCK:
        dt=now-position_lock_start_time;
        if (hasLock() || dt > POSITION_LOCK_TIMEOUT) {
            DEBUG_PRINT("Position lock acquired, ready for take off..\n");
            // ledseqRun(&seq_lock);
            state = STATE_WAIT_FOR_TAKE_OFF;
        }
        break;
        case STATE_WAIT_FOR_TAKE_OFF:
            if (takeOffWhenReady || peerLocalizationGetNumNeighbors() > 0 ) {
                takeOffWhenReady = false;
                padX = getX();
                padY = getY();
                padZ = getZ();
                DEBUG_PRINT("Base position: (%f, %f, %f)\n", (double)padX, (double)padY, (double)padZ);

                terminateTrajectoryAndLand = false;
                DEBUG_PRINT("Taking off...\n");
                crtpCommanderHighLevelTakeoff(padZ + TAKE_OFF_HEIGHT, 1.0);
                state = STATE_TAKING_OFF;
            }
        break;
        case STATE_TAKING_OFF:
            if (crtpCommanderHighLevelIsTrajectoryFinished()) {
                DEBUG_PRINT("Hovering, waiting for command to start\n");
                // ledseqStop(&seq_lock);
                state = STATE_HOVERING;
                DEBUG_PRINT("Enabling Collision Avoidance\n");
                enableCollisionAvoidance();

                hovering_start_time = now;
            }
            break;
        case STATE_HOVERING:
            dt = now - hovering_start_time ;
            if (terminateTrajectoryAndLand || dt > HOVERING_TIME) {
                crtpCommanderHighLevelGoTo(padX, padY, padZ + LANDING_HEIGHT, 0.0, GO_TO_PAD_DURATION, false);
                state = STATE_GOING_TO_PAD;
                break;
            }
            
            uint8_t otherId =  my_id == 4 ? 6 : 4;

            if (my_id==9)
                break;
            
            if (peerLocalizationIsIDActive(otherId)) {
                peerLocalizationOtherPosition_t * togo = peerLocalizationGetPositionByID(otherId);
                
                crtpCommanderHighLevelGoTo((*togo).pos.x, (*togo).pos.y,(*togo).pos.z, 0.0,DELTA_DURATION,false);
                state=STATE_GOING_TO_DELTA_POINT;
            }
            
                 
            break;
        case STATE_GOING_TO_DELTA_POINT:
            if (crtpCommanderHighLevelIsTrajectoryFinished()) {
                state = STATE_HOVERING;
            }
            break;
        case STATE_GOING_TO_PAD:
            if (crtpCommanderHighLevelIsTrajectoryFinished()) {
                DEBUG_PRINT("Over pad, stabalizing position\n");
                stabilizeEndTime = now + 5000;
                state = STATE_WAITING_AT_PAD;
            }
            break;
        case STATE_WAITING_AT_PAD:
            if (now > stabilizeEndTime || ((fabs(padX - getX()) < MAX_PAD_ERR) && (fabs(padY - getY()) < MAX_PAD_ERR))) {
                if (now > stabilizeEndTime) {
                DEBUG_PRINT("Warning: timeout!\n");
                }

                DEBUG_PRINT("Landing...\n");
                crtpCommanderHighLevelLand(padZ, 1.0);
                state = STATE_LANDING;
            }
            break;
        case STATE_LANDING:
            if (crtpCommanderHighLevelIsTrajectoryFinished()) {
                DEBUG_PRINT("Landed. Feed me!\n");
                crtpCommanderHighLevelStop();
                landingTimeCheckCharge = now + 3000;
                state = STATE_CHECK_CHARGING;
            }
            break;
        case STATE_CHECK_CHARGING:
            if (now > landingTimeCheckCharge) {
                DEBUG_PRINT("isCharging: %d\n", isCharging());
                if (isCharging()) {
                // ledseqRun(&seq_lock);
                state = STATE_WAIT_FOR_TAKE_OFF;
                } else {
                DEBUG_PRINT("Not charging. Try to reposition on pad.\n");
                crtpCommanderHighLevelTakeoff(padZ + LANDING_HEIGHT, 1.0);
                state = STATE_REPOSITION_ON_PAD;
                }
            }
            break;
        case STATE_REPOSITION_ON_PAD:
            if (crtpCommanderHighLevelIsTrajectoryFinished()) {
                DEBUG_PRINT("Over pad, stabalizing position\n");
                crtpCommanderHighLevelGoTo(padX, padY, padZ + LANDING_HEIGHT, 0.0, 1.5, false);
                state = STATE_GOING_TO_PAD;
            }
            break;
        case STATE_CRASHED:
            crtpCommanderHighLevelStop();
            DEBUG_PRINT("Crashed,  seq_crash_running : %d\n",seq_crash_running);

            if (seq_crash_running!=1){
                ledseqRun(&seq_crash);
                seq_crash_running=1;
            }
        break;
        
        default:
        break;
    }
}

void appMain()
{
    if (isInit) {
        return;
    }

    DEBUG_PRINT("Waiting for activation ...\n");
    // Get log and param ids
    logIdStateEstimateX = logGetVarId("stateEstimate", "x");
    logIdStateEstimateY = logGetVarId("stateEstimate", "y");
    logIdStateEstimateZ = logGetVarId("stateEstimate", "z");
    logIdKalmanVarPX = logGetVarId("kalman", "varPX");
    logIdKalmanVarPY = logGetVarId("kalman", "varPY");
    logIdKalmanVarPZ = logGetVarId("kalman", "varPZ");
    logIdPmState = logGetVarId("pm", "state");
    logIdlighthouseEstBs0Rt = logGetVarId("lighthouse", "estBs0Rt");
    logIdlighthouseEstBs1Rt = logGetVarId("lighthouse", "estBs1Rt");
    paramIdStabilizerController = paramGetVarId("stabilizer", "controller");
    paramIdCommanderEnHighLevel = paramGetVarId("commander", "enHighLevel");
    paramIdLighthouseMethod = paramGetVarId("lighthouse", "method");
    paramIdCollisionAvoidance = paramGetVarId("colAv", "enable");


    ledseqRegisterSequence(&seq_estim_stuck);    
    ledseqRegisterSequence(&seq_crash);

    initPacket();

    enableHighlevelCommander();
    
    // Register the callback function so that the CF can receive packets as well.
    p2pRegisterCB(p2pcallbackHandler);
    
    previous[0]=0.0f;
    previous[1]=0.0f;
    previous[2]=0.0f;

    sendPosTimer = xTimerCreate("SendPosTimer", M2T(BROADCAST_PERIOD_MS), pdTRUE, NULL, sendPosition);
    xTimerStart(sendPosTimer, 20);

    stateTransitionTimer = xTimerCreate("AppTimer", M2T(CALC_NEXT_PERIOD_MS), pdTRUE, NULL, stateTransition);
    xTimerStart(stateTransitionTimer, 20);

    isInit = true;

    
}


PARAM_GROUP_START(app)
  PARAM_ADD(PARAM_UINT8, takeoff, &takeOffWhenReady)
  PARAM_ADD(PARAM_UINT8, stop, &terminateTrajectoryAndLand)
  PARAM_ADD(PARAM_FLOAT, offsx, &offset.x)
  PARAM_ADD(PARAM_FLOAT, offsy, &offset.y)
  PARAM_ADD(PARAM_FLOAT, offsz, &offset.z)
PARAM_GROUP_STOP(app)

LOG_GROUP_START(app)
  LOG_ADD(LOG_UINT8, state, &state)
LOG_GROUP_STOP(app)
