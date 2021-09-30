#include <string.h>
#include <stdlib.h>
#include "FreeRTOS.h"
#include "timers.h"
#include "radiolink.h"
#include "protocol.h"
#include "configblock.h"


#include "tower.h"

#define DEBUG_MODULE "TOWER"
#include "debug.h"

#if 1
  #define DBG_FLOW(fmt, ...)  DEBUG_PRINT(fmt, ## __VA_ARGS__)
#else
  #define DBG_FLOW(...)
#endif


typedef struct {
  uint8_t nodeId1;
  uint32_t endTime1;
  uint8_t nodeId2;
  uint32_t endTime2;
} SwarmState;


static void initiatorSendProposition(const uint32_t proposalNr);
static void initiatorSendStateUpdateRequest(const uint32_t proposalNr, const SwarmState* newState);
static void sendTowerMessage(const uint8_t* msg, const uint8_t size);

static void p2pRxCallback(P2PPacket *packet);
static void acceptorHandleProposition(const Proposition* data);
static void initiatorHandlePromise(const Promise* data);
static void acceptorHandleStateUpdateRequest(const StateUpdateRequest* data);
static void learnerHandleStateUpdateAccept(const StateUpdateAccept* data);

static void setSwarmStateFromDeltaState(SwarmState* target, const DeltaState* source);
static void setDeltaStateFromSwarmState(DeltaState* target, const SwarmState* source);


// The node id is initialized from the last byte of the radio address
// Node ids for the members of the swarm are expected to be in the range 0 - 8
// The id is used for identification but also to time slot P2P TX times
static uint8_t nodeId = 0;

// Total nr of nodes
static const int NODE_COUNT = 9;
// Nr of nodes required for majority
static const int MAJORITY_COUNT = 5;

static const uint32_t TX_DELAY_SLOT_TIME = T2M(20);
static uint32_t responseTxDelay = 0;
static uint32_t getTxSlotTick();

static const int NO_FLIGHT_SLOT_EMPTY = -1;
static int findEmptyFlightSlot(uint32_t now, const SwarmState* state);
static bool planFlight(uint32_t now, SwarmState* newState);

// Distributed concensus data ****************************************
// General data ======================================================
// The highest propsal id seen so far
static uint32_t highestSeenId = 0;
// Latest known concensus state in the swarm
static SwarmState latestKnownConcensusState;
static uint32_t latestKnownConcensusStateProposalNr = 0;

// Learner states ====================================================
// Workspace for collecting latest known concensus state.
// Updated when receiving StateUpdateAccept messages. When a majority is reached we
// have concensus and the state is copied to latestKnownConcensusState
static int learnerConcensusStateCount = 0;
static uint32_t learnerConcensusStatePropositionNr = 0;
static uint32_t learnerAcceptVoteEndTime = 0;

// Acceptor state ===================================================
// The highest proposal nr the acceptor has promised to accept
static uint32_t acceptorPromisedProposalNr = 0;

// The state the acceptor has commited
static SwarmState acceptorCommitedState;
// The proposal nr of the commited state
static uint32_t acceptorCommitedProposalNr = 0;

static void acceptorSendPromise();
static void acceptorSendStateUpdateAccept();

// Initiator state ==================================================
// The current propsal nr used by the initiator
static uint32_t initiatorCurrentProposalNr = 0;
// Nr of promises received for the initiatorCurrentProposalNr
int initiatorPromiseCount = 0;
// The time when we stop waiting for more votes related to the promise
static uint32_t initiatorPromiseVoteEndTime;
static const uint32_t VOTE_TIME = TX_DELAY_SLOT_TIME * (NODE_COUNT + 2);
static void initiateNewProposition(const uint32_t now);

// The latest (with the highest proposal nr) commited state returned by acceptors
static SwarmState initiatorPromiseState;
// The proposal nr associated with the initiatorPromiseState
static uint32_t initiatorHighestProposalNr;

static uint32_t generateProposalNr();

// Tower/protocol state machine
enum TowerState {
  // Initialization
  STATE_IDLE = 0,
  STATE_CHECK_STATUS,
  STATE_WAIT_FOR_PROMISE,
  STATE_PLAN_FLIGHT,
  STATE_WAIT_FOR_ACCEPTED_STATE_UPDATE,
};

static enum TowerState towerState;

const static uint32_t WAIT_FOR_CHARGE_CHECK_TIME = M2T(6000);
const static uint32_t WAIT_FOR_RETRY_INITIATE_TIME = M2T(400);
const uint32_t PEER_INITIATED_TRANSACTION_HOLD_BACK_TIME = M2T(600);
const uint32_t PROPOSAL_FAILED_HOLD_BACK_TIME = PEER_INITIATED_TRANSACTION_HOLD_BACK_TIME + M2T(300);


static uint32_t nextPromiseTxTime = 0;
static Promise nextPromise;
static uint32_t nextStateUpdateAcceptTxTime = 0;
static uint32_t towerHoldbackTime = 0;
static uint32_t goToIdle(const uint32_t now, const uint32_t minimumDelay);
static void holdBackNewInitiations();



void initTower() {
  DEBUG_PRINT("Tower initialization\n");

  uint64_t address = configblockGetRadioAddress();
  nodeId =(uint8_t)((address) & 0x00000000ff);
  DEBUG_PRINT("I have node id %d\n", nodeId);
  srand(nodeId);

  responseTxDelay = TX_DELAY_SLOT_TIME * (nodeId + 2);

  // Randomize startup
  towerState = goToIdle(xTaskGetTickCount(), 0);

  p2pRegisterCB(p2pRxCallback);
}

// Called every 10 ms
void towerTimerCb(xTimerHandle timer) {
  // First make sure we respond to incoming messages
  const uint32_t now = xTaskGetTickCount();
  if (0 != nextPromiseTxTime && now > nextPromiseTxTime) {
    acceptorSendPromise();
    nextPromiseTxTime = 0;

    // Only transmit one packet each slot
    return;
  }
  if (0 != nextStateUpdateAcceptTxTime && now > nextStateUpdateAcceptTxTime) {
    acceptorSendStateUpdateAccept();
    nextStateUpdateAcceptTxTime = 0;

    // Only transmit one packet each slot
    return;
  }

  // Execute the state machine to see if need to take any action
  enum TowerState oldTowerdState = towerState;
  switch(towerState) {
    case STATE_IDLE:
      if (now > towerHoldbackTime) {
        towerState = STATE_CHECK_STATUS;
      }
      break;
    case STATE_CHECK_STATUS:
      {
        // TODO Implement check for battery voltage
        bool isBatteryCharged = true;
        if (isBatteryCharged) {
          bool isAFlightSlotEpty = (findEmptyFlightSlot(now, &latestKnownConcensusState) != NO_FLIGHT_SLOT_EMPTY);
          if (isAFlightSlotEpty) {
            DBG_FLOW("Newproposal at %lu\n", now);
            initiateNewProposition(now);
            towerState = STATE_WAIT_FOR_PROMISE;
          } else {
            towerState = goToIdle(now, WAIT_FOR_RETRY_INITIATE_TIME);
          }
        } else {
          towerState = goToIdle(now, WAIT_FOR_CHARGE_CHECK_TIME);
        }
      }
      break;
    case STATE_WAIT_FOR_PROMISE:
      if (now > initiatorPromiseVoteEndTime) {
        if (initiatorPromiseCount >= MAJORITY_COUNT) {
          // Got majority!
          if (initiatorHighestProposalNr == latestKnownConcensusStateProposalNr) {
            // Our know accepted state matches what we got from acceptors, we are good to go!
            towerState = STATE_PLAN_FLIGHT;
          } else {
            // Seems as our known accepted state is not up to date, resend the newer one and start over.
            DBG_FLOW("Promise majority, bad state. %lu VS %lu\n", initiatorHighestProposalNr, latestKnownConcensusStateProposalNr);
            DBG_FLOW("Resending state on %lu\n", initiatorCurrentProposalNr);
            initiatorSendStateUpdateRequest(initiatorCurrentProposalNr, &initiatorPromiseState);
            towerState = goToIdle(now, WAIT_FOR_RETRY_INITIATE_TIME);
          }
        } else {
          // No majority, go back to idle to start over
          towerState = goToIdle(now, PROPOSAL_FAILED_HOLD_BACK_TIME);
          DBG_FLOW("No promise, %d\n", initiatorPromiseCount);
        }
      }
      break;
    case STATE_PLAN_FLIGHT:
      {
        SwarmState newState;
        bool planSuccess = planFlight(now, &newState);
        if (planSuccess) {
          DBG_FLOW("Flight plan success\n");
          initiatorSendStateUpdateRequest(initiatorCurrentProposalNr, &newState);
          learnerAcceptVoteEndTime = now + VOTE_TIME;
          towerState = STATE_WAIT_FOR_ACCEPTED_STATE_UPDATE;
        } else {
          DBG_FLOW("Flight plan failed\n");
          towerState = goToIdle(now, WAIT_FOR_RETRY_INITIATE_TIME);
        }
      }
      break;
    case STATE_WAIT_FOR_ACCEPTED_STATE_UPDATE:
      if (latestKnownConcensusStateProposalNr == initiatorCurrentProposalNr) {
        DBG_FLOW("Flight plan accepted\n");
        // TODO if majoity take off
        towerState = goToIdle(now, WAIT_FOR_RETRY_INITIATE_TIME);
      } else {
        if (now > learnerAcceptVoteEndTime) {
          DBG_FLOW("No majority for flight plan\n");
          towerState = goToIdle(now, WAIT_FOR_RETRY_INITIATE_TIME);
        }
      }
      break;
    default:
      // Should not get here, ignore it
      break;
  }

  if (oldTowerdState != towerState) {
    DBG_FLOW("New state: %d\n", towerState);
  }
}

static void initiatorSendProposition(const uint32_t proposalNr) {
  static Proposition msg = {
    .msgType = 1
  };

  msg.nodeId = nodeId;
  msg.proposalNr = proposalNr;

  sendTowerMessage((uint8_t*)&msg, sizeof(msg));
}

static void acceptorSendPromise() {
  nextPromise.msgType = MSG_TYPE_PROMISE;
  nextPromise.nodeId = nodeId;
  nextPromise.previousProposalNr = acceptorCommitedProposalNr;
  setDeltaStateFromSwarmState(&nextPromise.currentState, &acceptorCommitedState);
  nextPromise.propositionAccepted = true;
  // proposalNr filled in earlier

  sendTowerMessage((uint8_t*)&nextPromise, sizeof(nextPromise));
}

static void initiatorSendStateUpdateRequest(const uint32_t proposalNr, const SwarmState* newState) {
  static StateUpdateRequest msg = {
    .msgType = MSG_TYPE_STATE_UPDATE_REQUEST
  };

  msg.nodeId = nodeId;
  msg.proposalNr = proposalNr;
  setDeltaStateFromSwarmState(&msg.newState, newState);

  sendTowerMessage((uint8_t*)&msg, sizeof(msg));
}

static void acceptorSendStateUpdateAccept() {
  StateUpdateAccept nextStateUpdateAccept;

  nextStateUpdateAccept.msgType = MSG_TYPE_STATE_UPDATE_ACCEPT;
  nextStateUpdateAccept.nodeId = nodeId;
  nextStateUpdateAccept.proposalNr = acceptorCommitedProposalNr;
  nextStateUpdateAccept.updateAccepted = true;
  setDeltaStateFromSwarmState(&nextStateUpdateAccept.newState, &acceptorCommitedState);

  sendTowerMessage((uint8_t*)&nextStateUpdateAccept, sizeof(nextStateUpdateAccept));
}

static void sendTowerMessage(const uint8_t* msg, const uint8_t size) {
  // Not re-entrant safe
  static P2PPacket pk;

  pk.port = 0;
  pk.size = size;
  memcpy(pk.data, msg, size);
  radiolinkSendP2PPacketBroadcast(&pk);

  // Feed the message into our own handling flow as well
  p2pRxCallback(&pk);
}


static uint32_t generateProposalNr() {
  const uint32_t currentBase = highestSeenId & 0xffffff00;
  const uint32_t newBase = currentBase + 0x00000100;
  const uint32_t newProposalId = newBase | nodeId;

  return newProposalId;
}

// TODO krri Protect data to be reentrant safe
static void p2pRxCallback(P2PPacket *packet) {
  const uint8_t msgType = packet->data[0];
  uint32_t proposalNr = 0;
  memcpy(&proposalNr, &packet->data[2], sizeof(uint32_t));

  if (proposalNr > highestSeenId) {
    highestSeenId = proposalNr;
  }

  switch(msgType) {
    case MSG_TYPE_PROPOSITION:
      holdBackNewInitiations();
      acceptorHandleProposition((Proposition*)packet->data);
      break;
    case MSG_TYPE_PROMISE:
      initiatorHandlePromise((Promise*)packet->data);
      break;
    case MSG_TYPE_STATE_UPDATE_REQUEST:
      holdBackNewInitiations();
      acceptorHandleStateUpdateRequest((StateUpdateRequest*)packet->data);
      break;
    case MSG_TYPE_STATE_UPDATE_ACCEPT:
      learnerHandleStateUpdateAccept((StateUpdateAccept*)packet->data);
      break;
    default:
      // Unhandled message type
      break;
  }
}

static void acceptorHandleProposition(const Proposition* data) {
  if (data->proposalNr > acceptorPromisedProposalNr) {
    acceptorPromisedProposalNr = data->proposalNr;
    nextPromise.proposalNr = data->proposalNr;

    // Initiate transmission of Promise message
    nextPromiseTxTime = getTxSlotTick();
  }
}

static void initiatorHandlePromise(const Promise* data) {
  if (data->propositionAccepted) {
    if (data->proposalNr == initiatorCurrentProposalNr) {
      initiatorPromiseCount++;

      const uint32_t now = xTaskGetTickCount();
      DBG_FLOW("Pr %lu %i %i\n", now, data->nodeId, initiatorPromiseCount);

      if (data->previousProposalNr > initiatorHighestProposalNr) {
        initiatorHighestProposalNr = data->previousProposalNr;
        setSwarmStateFromDeltaState(&initiatorPromiseState, &data->currentState);
      }
    }
  }
}

static void acceptorHandleStateUpdateRequest(const StateUpdateRequest* data) {
  if (data->proposalNr >= acceptorPromisedProposalNr) {
    if (data->proposalNr > acceptorCommitedProposalNr) {
      acceptorCommitedProposalNr = data->proposalNr;
      setSwarmStateFromDeltaState(&acceptorCommitedState, &data->newState);

      // Initiate transmission of StateUpdateAccept message
      nextStateUpdateAcceptTxTime = getTxSlotTick();
    }
  }
}

static void learnerHandleStateUpdateAccept(const StateUpdateAccept* data) {
  if (data->updateAccepted) {
    if (data->proposalNr > learnerConcensusStatePropositionNr) {
      learnerConcensusStatePropositionNr = data->proposalNr;
      learnerConcensusStateCount = 0;
      DBG_FLOW("New learner id %lu\n", learnerConcensusStatePropositionNr);
    }

    if (data->proposalNr == learnerConcensusStatePropositionNr) {
      learnerConcensusStateCount++;
      const uint32_t now = xTaskGetTickCount();
      DBG_FLOW("Learner count %lu %i %i\n", now, data->nodeId, learnerConcensusStateCount);
      if (learnerConcensusStateCount == MAJORITY_COUNT) {
        // We have a majority! Save the state as the new concensus.
        setSwarmStateFromDeltaState(&latestKnownConcensusState, &data->newState);
        latestKnownConcensusStateProposalNr = data->proposalNr;
        DBG_FLOW("New concensus! Id: %lu\n", latestKnownConcensusStateProposalNr);
      }
    }
  }
}

static void setSwarmStateFromDeltaState(SwarmState* target, const DeltaState* source) {
  const uint32_t now = xTaskGetTickCount();

  target->nodeId1 = source->nodeId1;
  target->endTime1 = source->timeRemaining1 + now;

  target->nodeId2 = source->nodeId2;
  target->endTime2 = source->timeRemaining2 + now;
}

static void setDeltaStateFromSwarmState(DeltaState* target, const SwarmState* source) {
  const uint32_t now = xTaskGetTickCount();

  target->nodeId1 = source->nodeId1;
  if (source->endTime1 > now) {
    target->timeRemaining1 = source->endTime1 - now;
  } else {
    target->timeRemaining1 = 0;
  }

  target->nodeId2 = source->nodeId2;
  if (source->endTime2 > now) {
    target->timeRemaining2 = source->endTime2 - now;
  } else {
    target->timeRemaining2 = 0;
  }
}


static void initiateNewProposition(const uint32_t now) {
  initiatorCurrentProposalNr = generateProposalNr();
  initiatorPromiseCount = 0;
  initiatorPromiseVoteEndTime = now + VOTE_TIME;

  initiatorSendProposition(initiatorCurrentProposalNr);
}

static uint32_t getTxSlotTick() {
  const uint32_t now = xTaskGetTickCount();
  const uint32_t txTime = now + responseTxDelay;

  return txTime;
}

static uint32_t randomDelay(const uint32_t maxDelayMs) {
  return (uint32_t)(M2T(maxDelayMs) * (float)rand() / (float)RAND_MAX);
}

static uint32_t goToIdle(const uint32_t now, const uint32_t minimumDelay) {
  const uint32_t maxRandomAdditionMs = 100;
  towerHoldbackTime = now + minimumDelay + randomDelay(maxRandomAdditionMs);

  return STATE_IDLE;
}

static void holdBackNewInitiations() {
  const uint32_t maxRandomAdditionMs = 100;

  if (towerState == STATE_IDLE) {
    const uint32_t now = xTaskGetTickCount();
    const uint32_t minimumHoldbackTime = now + PEER_INITIATED_TRANSACTION_HOLD_BACK_TIME + randomDelay(maxRandomAdditionMs);
    if (minimumHoldbackTime > towerHoldbackTime) {
      towerHoldbackTime = minimumHoldbackTime;
    }
  }
}

static int findEmptyFlightSlot(uint32_t now, const SwarmState* state) {
  if (state->endTime1 <= now) {
    return 0;
  }

  // TODO Enable second slot
  // if (state->endTime2 <= now) {
  //   return 1;
  // }

  return NO_FLIGHT_SLOT_EMPTY;
}

static bool planFlight(uint32_t now, SwarmState* newState) {
  // TODO Plan flight (based on initiatorPromiseState)
  // TODO impelement for real. Use dummy state for testing
  // Use initiatorPromiseState, we are sure it has not been updated
  // since the majority was received. It is possible (but unlikely)
  // that latestKnownConcensusState had been modified
  const SwarmState* currentState = &initiatorPromiseState;

  bool result = false;

  if (findEmptyFlightSlot(now, currentState) == 0) {
    newState->nodeId1 = nodeId;
    newState->endTime1 = now + 5000;
    newState->nodeId2 = 0;
    newState->endTime2 = now;

    result = true;
  }

  return result;
}
