#include "mot_pap.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#include "board.h"
#include "FreeRTOS.h"
#include "task.h"

#include "ad2s1210.h"
#include "debug.h"
#include "relay.h"
#include "tmr.h"

// Freqs expressed in Khz
static const uint32_t mot_pap_free_run_freqs[] = { 0, 25, 25, 25, 50, 75, 75,
		100, 125 };

/**
 * @brief	corrects possible offsets of RDC alignment.
 * @param 	pos		: current RDC position
 * @param 	offset	: RDC value for 0 degrees
 * @return	the offset corrected position
 */
uint16_t mot_pap_offset_correction(uint16_t pos, uint16_t offset,
		uint8_t resolution)
{
	int32_t corrected = pos - offset;
	if (corrected < 0)
		corrected = corrected + (int32_t) (1 << resolution);
	return (uint16_t) corrected;
}

/**
 * @brief	checks if software limits are reached
 * @param 	me			: struct mot_pap pointer
 * @return 	nothing
 * @note	also sets stalled to false for this instance
 */
void mot_pap_init_limits(struct mot_pap *me)
{
	me->posAct = mot_pap_offset_correction(ad2s1210_read_position(me->rdc),
			me->offset, me->rdc->resolution);
	me->cwLimitReached = false;
	me->ccwLimitReached = false;

//	if (me->posAct >= me->cwLimit) {
//		me->cwLimitReached = true;
//	}
//
//	if (me->posAct <= me->ccwLimit) {
//		me->ccwLimitReached = true;
//	}
}

/**
 * @brief	returns the direction of movement depending if the error is positive or negative
 * @param 	error : the current position error in closed loop positioning
 * @return	MOT_PAP_DIRECTION_CW if error is positive
 * @return	MOT_PAP_DIRECTION_CCW if error is negative
 */
enum mot_pap_direction mot_pap_direction_calculate(int32_t error)
{
	return error < 0 ? MOT_PAP_DIRECTION_CW : MOT_PAP_DIRECTION_CCW;
}

/**
 * @brief 	checks if the required FREE RUN speed is in the allowed range
 * @param 	speed : the requested speed
 * @return	true if speed is in the allowed range
 */
bool mot_pap_free_run_speed_ok(uint32_t speed)
{
	return ((speed > 0) && (speed <= MOT_PAP_MAX_SPEED_FREE_RUN));
}

/**
 * @brief 	checks if a movement in the desired direction is possible
 * @param 	dir			    : the desired direction of movement
 * @param 	cwLimitReached  : true if the CW limit has been reached
 * @param 	ccwLimitReached : true if the CCW limit has been reached
 * @return  true if the direction of movement is not in the same
 * 			direction of the limit that has already been reached
 */
bool mot_pap_movement_allowed(enum mot_pap_direction dir,
bool cwLimitReached, bool ccwLimitReached)
{
	return ((dir == MOT_PAP_DIRECTION_CW && !cwLimitReached)
			|| (dir == MOT_PAP_DIRECTION_CCW && !ccwLimitReached));
}

/**
 * @brief 	supervise motor movement for limits, stall, position reached in closed loop
 * @param 	me			: struct mot_pap pointer
 * @return  nothing
 * @note	to be called by the deferred interrupt task handler
 */
void mot_pap_supervise(struct mot_pap *me)
{
	int32_t error;
	bool already_there;
	enum mot_pap_direction dir;

	me->posAct = mot_pap_offset_correction(ad2s1210_read_position(me->rdc),
			me->offset, me->rdc->resolution);

	me->cwLimitReached = false;
	me->ccwLimitReached = false;

//	if ((me->dir == MOT_PAP_DIRECTION_CW)
//			&& (me->posAct >= (int32_t) me->cwLimit)) {
//		me->cwLimitReached = true;
//		tmr_stop(&(me->tmr));
//		lDebug(Warn, "%s: limit CW reached", me->name);
//		goto cont;
//	}
//
//	if ((me->dir == MOT_PAP_DIRECTION_CCW)
//			&& (me->posAct <= (int32_t) me->ccwLimit)) {
//		me->ccwLimitReached = true;
//		tmr_stop(&(me->tmr));
//		lDebug(Warn, "%s: limit CCW reached", me->name);
//		goto cont;
//	}

	if (stall_detection) {
		if (abs((int) (me->posAct - me->last_pos)) < MOT_PAP_STALL_THRESHOLD) {

			me->stalled_counter++;
			if (me->stalled_counter >= MOT_PAP_STALL_MAX_COUNT) {
				me->stalled = true;
				tmr_stop(&(me->tmr));
				relay_main_pwr(0);
				lDebug(Warn, "%s: stalled", me->name);
				goto cont;
			}
		} else {
			me->stalled_counter = 0;
		}
	}

	if (me->type == MOT_PAP_TYPE_CLOSED_LOOP) {
		error = me->posCmd - me->posAct;
		already_there = (abs((int) error) < MOT_PAP_POS_THRESHOLD);

		if (already_there) {
			me->type = MOT_PAP_TYPE_STOP;
			tmr_stop(&(me->tmr));
			lDebug(Info, "%s: position reached", me->name);
		} else {
			dir = mot_pap_direction_calculate(error);
			if (me->dir != dir) {
				tmr_stop(&(me->tmr));
				vTaskDelay(pdMS_TO_TICKS(MOT_PAP_DIRECTION_CHANGE_DELAY_MS));
				me->dir = dir;
				me->gpios.direction(me->dir);
				tmr_start(&(me->tmr));
			}
		}
	}
	cont: me->last_pos = me->posAct;
}

/**
 * @brief	if allowed, starts a free run movement
 * @param 	me			: struct mot_pap pointer
 * @param 	direction	: either MOT_PAP_DIRECTION_CW or MOT_PAP_DIRECTION_CCW
 * @param 	speed		: integer from 0 to 8
 * @return 	nothing
 */
void mot_pap_move_free_run(struct mot_pap *me, enum mot_pap_direction direction,
		uint32_t speed)
{
	bool allowed, speed_ok;

//	allowed = mot_pap_movement_allowed(direction, me->cwLimitReached,
//			me->ccwLimitReached);
	allowed = true;
	speed_ok = mot_pap_free_run_speed_ok(speed);

	if (allowed && speed_ok) {
		if ((me->dir != direction) && (me->type != MOT_PAP_TYPE_STOP)) {
			tmr_stop(&(me->tmr));
			vTaskDelay(pdMS_TO_TICKS(MOT_PAP_DIRECTION_CHANGE_DELAY_MS));
		}
		me->type = MOT_PAP_TYPE_FREE_RUNNING;
		me->dir = direction;
		me->gpios.direction(me->dir);
		me->freq = mot_pap_free_run_freqs[speed] * 1000;

		tmr_set_freq(&(me->tmr), me->freq);
		tmr_start(&(me->tmr));
		lDebug(Info, "%s: FREE RUN, speed: %u, direction: %s", me->name,
				me->freq, me->dir == MOT_PAP_DIRECTION_CW ? "CW" : "CCW");
	} else {
//		if (!allowed)
//			lDebug(Warn, "%s: movement out of bounds %s", me->name,
//					direction == MOT_PAP_DIRECTION_CW ? "CW" : "CCW");
		if (!speed_ok)
			lDebug(Warn, "%s: chosen speed out of bounds %u", me->name, speed);
	}
}

/**
 * @brief	if allowed, starts a closed loop movement
 * @param 	me			: struct mot_pap pointer
 * @param 	setpoint	: the resolver value to reach
 * @return 	nothing
 */
void mot_pap_move_closed_loop(struct mot_pap *me, uint16_t setpoint)
{
	int32_t error;
	bool already_there;
	enum mot_pap_direction dir;

//	if ((setpoint > me->cwLimit) | (setpoint < me->ccwLimit)) {
//		lDebug(Warn, "%s: movement out of bounds", me->name);
//	} else {
		me->posCmd = setpoint;
		lDebug(Info, "%s: CLOSED_LOOP posCmd: %u posAct: %u", me->name,
				me->posCmd, me->posAct);

		//calcular error de posición
		error = me->posCmd - me->posAct;
		already_there = (abs(error) < MOT_PAP_POS_THRESHOLD);

		if (already_there) {
			tmr_stop(&(me->tmr));
			lDebug(Info, "%s: already there", me->name);
		} else {
			dir = mot_pap_direction_calculate(error);
//			if (mot_pap_movement_allowed(dir, me->cwLimitReached,
//					me->ccwLimitReached)) {
				if ((me->dir != dir) && (me->type != MOT_PAP_TYPE_STOP)) {
					tmr_stop(&(me->tmr));
					vTaskDelay(
							pdMS_TO_TICKS(MOT_PAP_DIRECTION_CHANGE_DELAY_MS));
				}
				me->type = MOT_PAP_TYPE_CLOSED_LOOP;
				me->dir = dir;
				me->gpios.direction(me->dir);
				me->freq = MOT_PAP_MAX_FREQ;
				tmr_set_freq(&(me->tmr), me->freq);
//				lDebug(Info, "%s: CLOSED LOOP, speed: %u, direction: %s",
//						me->name, me->freq,
//						me->dir == MOT_PAP_DIRECTION_CW ? "CW" : "CCW");
				if (!tmr_started(&(me->tmr))) {
					tmr_start(&(me->tmr));
				}
//			} else {
//				lDebug(Warn, "%s: movement out of bounds %s", me->name,
//						dir == MOT_PAP_DIRECTION_CW ? "CW" : "CCW");
//			}
		}
	}
//}

/**
 * @brief	if there is a movement in process, stops it
 * @param 	me	: struct mot_pap pointer
 * @return 	nothing
 */
void mot_pap_stop(struct mot_pap *me)
{
	me->type = MOT_PAP_TYPE_STOP;
	tmr_stop(&(me->tmr));
	lDebug(Info, "%s: STOP", me->name);
}

/**
 * @brief 	function called by the timer ISR to generate the output pulses
 * @param 	me : struct mot_pap pointer
 */
void mot_pap_isr(struct mot_pap *me)
{
	BaseType_t xHigherPriorityTaskWoken;

	if (me->dir != me->last_dir) {
		me->half_pulses = 0;
		me->last_dir = me->dir;
	}

	me->gpios.pulse();

	if (++(me->half_pulses) == MOT_PAP_SUPERVISOR_RATE) {
		me->half_pulses = 0;
		xHigherPriorityTaskWoken = pdFALSE;
		xSemaphoreGiveFromISR(me->supervisor_semaphore,
				&xHigherPriorityTaskWoken);

		portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
	}
}

/**
 * @brief 	updates the current position from RDC
 * @param 	me : struct mot_pap pointer
 */
void mot_pap_update_position(struct mot_pap *me)
{
	me->posAct = mot_pap_offset_correction(ad2s1210_read_position(me->rdc),
			me->offset, me->rdc->resolution);
}

