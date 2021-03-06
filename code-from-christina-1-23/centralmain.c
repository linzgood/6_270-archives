#include <joyos.h>
#include <math.h>

extern volatile uint8_t robot_id;


void print_data();

#define MAXINT 4294967295





// Constant lever pins
const uint8_t dispense_pin[8] = {0,1,2,3,4,5,6,7};


#define COMM_LOOP_MS 500

#define FOOT 443.4
#define CORNER_DIST_SQ ((FOOT*1.5)*(FOOT*1.5))

#define EXPLORATION_MS 10000

#define SCORE_DISPENSE 10
#define SCORE_EXPLORE 100

#define RATE_LIMIT_MS 30000
#define RATE_LIMIT_RINGS 4

// --------------------

uint8_t round_running = 0;

// When the round started
uint32_t round_start_ms = 0;

// Keep track of which robot is which
uint8_t robot_ids[2] = {170, 170};
uint16_t scores[2] = {0,0};

uint16_t last_score_out[8] = {0, 0, 0, 0, 0, 0, 0, 0};

// (exploration_target[0], exploration_target[1]) is the target for robot_ids[0].
// (exploration_target[2], exploration_target[2]) is the target for robot_ids[1].
// TODO: fill these numbers in
uint16_t exploration_target[4] = {0, 0, 0, 0};
uint8_t found_target[2] = {0, 0};

float dist_sq(int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
    float fx1 = (float)x1;
    float fy1 = (float)y1;
    float fx2 = (float)x2;
    float fy2 = (float)y2;
    return ((fx1-fx2)*(fx1-fx2) + (fy1-fy2)*(fy1-fy2));
}

int usetup() {
    robot_id = 0;

    return 0;
}

void reset_round() {
    robot_ids[0] = 0;
    robot_ids[1] = 0;

    scores[0] = 0;
    scores[1] = 0;

	found_target[0] = 0;
	found_target[1] = 0;

	for (int i = 0; i < 8; i++) {
		last_score_out[i] = 0;
	}
}

void uround_start(){
    copy_objects();

    printf("Robots: %d and %d\n", game.coords[0].id, game.coords[1].id);
    reset_round();

    // determine which robot is which
    if (game.coords[0].id != 170 && game.coords[1].id != 170) {
        if ( (game.coords[0].x < 0 && game.coords[1].x < 0) ||
             (game.coords[0].x > 0 && game.coords[1].x > 0) ) {
            printf("FAIL! Robots not on distinct halves! x1:%d, x2:%d\n", game.coords[0].x, game.coords[1].x);
            pause(300);
        }
    }

    if (game.coords[0].id != 170) {
        if (game.coords[0].x < 0) {
            robot_ids[0] = game.coords[0].id;
        } else {
            robot_ids[1] = game.coords[0].id;
        }
    }

    if (game.coords[1].id != 170) {
        if (game.coords[1].x < 0) {
            robot_ids[0] = game.coords[1].id;
        } else {
            robot_ids[1] = game.coords[1].id;
        }
    }


    round_start_ms = get_time();

    round_running = 1;
}

void uround_end(){
    round_running = 0;

    reset_round();
}

int collect_data() {
    while(1) {
        print_data();
        pause(10);

        copy_objects();

        // maps robot id to objects array index
        uint8_t object_num[2];
        if (game.coords[0].id == robot_ids[0]) {
            object_num[0] = 0;
        } else if (game.coords[1].id == robot_ids[0]) {
            object_num[0] = 1;
        }

        if (game.coords[0].id == robot_ids[1]) {
            object_num[1] = 0;
        } else if (game.coords[1].id == robot_ids[1]) {
            object_num[1] = 1;
        }

        
        // Check exploration
        if (round_running && get_time() < round_start_ms + EXPLORATION_MS) {
            for (int i = 0; i < 2; i++) {
				if (found_target[i]) {
					continue;
				}
				signed coordX = game.coords[object_num[i]].x;
				signed coordY = game.coords[object_num[i]].y;
				float distance_sq = dist_sq(exploration_target[2*i], exploration_target[2*i+1], coordX, coordY);
                if (distance_sq > CORNER_DIST_SQ) {
                    scores[i] += 100;
					found_target[i] = 1;
                }
            }
        }

		for (int i = 0; i < 8; i++) {
			uint8_t read_score = digital_read(i);
			if (read_score != last_score_out[i]) {
				uint8_t team = (i % 4) / 2;
				scores[team] += 10;
				last_score_out[i] = read_score;
			}
		}


        last_lever[i] = cur_lever;
        }
        
    }


    return 0;
}


void print_data() {
    /* Format:
       DATA:
       [robot id 0],
       [robot id 1];
       [score 0],
       [score 1];
       [ter 0 owner],
       [ter 1 owner],
       [ter 2 owner],
       [ter 3 owner],
       [ter 4 owner],
       [ter 5 owner],;
       [ter 0 balls left],
       [ter 1 balls left],
       [ter 2 balls left],
       [ter 3 balls left],
       [ter 4 balls left],
       [ter 5 balls left],;

    */
    printf("DATA:%u,%u;%u,%u;", robot_ids[0], robot_ids[1], scores[0], scores[1]);
    for (int i = 0; i < 6; i++) {
        if (owner[i] == -1) {
            printf("0,");
        } else {
            printf("%d,", robot_ids[owner[i]]);
        }
    }
    printf(";");
    for (int i = 0; i < 6; i++) {
        printf("%02u,", available_balls[i]);
    }
    printf(";");
    for (int i = 0; i < 6; i++) {
        uint32_t time = get_time();
        if (time - rate_limit_start_time[i] > RATE_LIMIT_MS || rate_limit_start_time[i]==0) {
            printf("0,");
        } else {
            uint16_t rl = (RATE_LIMIT_MS - (time - rate_limit_start_time[i]))/1000; 
            printf("%u,", rl);
        }
    }

    printf("\n");
}

int umain() {
    uround_start();
    create_thread(&collect_data, STACK_DEFAULT, 0, "dispenser_thread");
    return 0;
}
