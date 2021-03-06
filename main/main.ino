#include <TinyGPS++.h>
#include <TM1637Display.h>

#define NDEBUG

////////////////////////////// TYPES //////////////////////////////

/**
 *  A goal is a password challenge (passwd)
 * that can only be solved in a specified location (lt, ln).
 */
typedef struct{
	/** latitude in degree. */
	double lt;
	/** longitude in degree. */
	double ln;
	/** Password of 4 hex-digits. */
	uint8_t passwd[4];
} goal;

/**
 * This type contains all information needed
 * to detect and process pressed buttons.
 */
typedef struct{
	/** Last time checked the button was pressed. */
	bool was_down;
	/** Event that fires when the button goes from not pressed to pressed. */
	bool* event;
	/** GPIO of the button. */
	int pin;
} button;

/**
 * A complex state that represents the whole circuid
 * and is used for transitions in the state machine (see #handler).
 */
typedef struct{
	/** Sattelite connection. */
	bool satcon;
	/** Location of current goal reached. */
	bool reached;
	/** Correct password entered. */
	bool passwd;
	/** Goals fetched. (see #goal) */
	bool fetched;
	/** Last #goal completed, you won the game! */
	bool completed;
	/** #button::event of reset button occured. */
	bool reset;
	/** #button::event of pos button occured. */
	bool pos;
	/** #button::event of count button occured. */
	bool count;
} state;

/**
 * The context is a summary of all values, flags and other data
 * that defines the system as a whole.
 * It gets passed from one #handler to the next.
 */
typedef struct{
	/** Current cursor position. */
	int input_pos;
	/** Digits that will be displayed (see #update_display). */
	int display_value[4];
	/** Distance to #context::current_goal (see #goal). */
	int distance;
	/** Index of current #goal in #context::goals. */
	int current_goal;
	/** Last index of #context:goals. */
	int final_goal;
	/** All goals that need to be achieved in ascending order to win the game. */
	goal goals[10];
	/** See #state. */
	state st;
	/** All buttons used in this circuid (see #button). */
	button btn[3];
} context;

/**
 * This is recursive type is part of the core logic of this sketch.
 * A hanlder can be executed and depending on the context that was
 * passed it returnes a new handler.
 * This is used to create a state machine where handlers are states
 * and returning the next handler is the transition (see #setup)
 */
typedef struct handler_wrapper{
	/**
	 * Execute to get the next handler.
	 *
	 * @param ct #context (to apply changes on and to decide which #handler will be returned next)
	 * @return #handler (next handler to execute in a infinite loop)
	 */
	struct handler_wrapper (*handle)(context* ct);
} handler;


///////////////////////////// GLOBALS /////////////////////////////

/** Earth radius in meters. */
#define EARTH_RADIUS 6371e3

/** Time to wait for a gps connection timeout (see #satcon_handler). */
#define GPS_TIMEOUT 4000
/** Maximum to wait for serial transmition of the GPS module. */
#define GPS_SERIAL_TIMEOUT 100
/** Read delay to avoid button bouncing. */
#define BTN_DELAY 100
/** Radius (in meters) of the target area for goals (#goal). */
#define DST_TOLERANCE 20
/** Brightness level for the 7-segment display array. */
#define BRIGHTNESS 0xF
/** Escape character for ' '. */
#define BLANK_C 0xFF
/** Excape character for '-'. */
#define DASH_C 0xFE

/** Dio pin for TM1637 chip. */
#define DIO 7
/** Clk pin for TM1637 chip. */
#define CLK 8
/** Reset button. */
#define RESET 0
/** Cursor position button. */
#define POS 1
/** Change count button. */
#define COUNT 4
/** Reset button. */
#define LED_R 2


//////////////////////////// UTILITIES ////////////////////////////

/** Interface for a connected 7-segment display array. */
TM1637Display display(CLK, DIO);
/** Interface for a connected gps module. */
TinyGPSPlus gps;

/**
 * Initiates a #context struct with default values.
 *
 * @param ct (to be initiated)
 */
void init_context(context* ct){
	ct->input_pos = 0;
	for(int i = 0; i < 4; i++){
		ct->display_value[i] = -1;
	}
	ct->distance = -1;
	ct->current_goal = 0;
	ct->final_goal = -1;
	ct->st = {false};
	ct->btn[0] = {false, &ct->st.reset, RESET};
	ct->btn[1] = {false, &ct->st.pos, POS};
	ct->btn[2] = {false, &ct->st.count, COUNT};
}

#ifndef NDEBUG
/**
 * This function can be used to debug state transitions (see @handler).
 *
 * @param next_handler (zero terminated string describing the next handler)
 * @param ct (#context in use)
 */
void print_status(const char* next_handler, const context* ct){
	Serial.println("Context:");
	Serial.print("\tinput_pos = ");
	Serial.println(ct->input_pos);
	Serial.print("\tdistance = ");
	Serial.println(ct->distance);
	Serial.print("\tcurrent_goal = {");
 	Serial.print(ct->goals[ct->current_goal].lt);
	Serial.print("; ");
	Serial.print(ct->goals[ct->current_goal].ln);
	Serial.println("; ****}");

	Serial.println("State:");
	Serial.print("\t[ ");
	if(ct->st.reset) Serial.print("r ");
	if(ct->st.satcon) Serial.print("s ");
	if(ct->st.reached) Serial.print("l ");
	if(ct->st.passwd) Serial.print("pw ");
	if(ct->st.fetched) Serial.print("f ");
	if(ct->st.completed) Serial.print("cm ");
	if(ct->st.pos) Serial.print("po ");
	if(ct->st.count) Serial.print("cn ");
	Serial.println("]");

	Serial.println();
	Serial.print("--> ");
	Serial.println(next_handler);
	Serial.println();
}
#else
void print_status(const char* next_handler, const context* ct){return;}
#endif

/**
 * Degree to rad conversion.
 *
 * @param degree (degree value to be converted)
 */
double deg_to_rad(const double degree){
	return PI * degree / 180;
}

/**
 * Set the digits of the 7-segment display array
 * to distance (in meters) to the current goal.
 *
 * @param ct (to obtain #context::distance and write to #context::display_value)
 */
void set_distance(context* ct){
	int8_t digit;
	bool dig_enable = false;
	int upper = 10000, lower = 1000;

	for(int i = 0; i < 4; i++){
		digit = (ct->distance % upper) / lower;

		if(!dig_enable){
			dig_enable = digit > 0;
		}

		ct->display_value[i] = dig_enable ? digit : BLANK_C;

		upper = lower;
		lower /= 10;
	}
}

/**
 * Obtain current location from GPS module's serial connection
 * and calculates distance to current goal.
 * <ul>
 *   <li>If its not possible to obtain location data (see #GPS_TIMEOUT)
 *   satcon wil be set to false (#state::satcon)</li>
 *   <li>If no goals have been fetched (#context::st::fetched) this function
 *   will just update satellite connection status but not the distance.</li>
 * </ul>
 *
 * @param ct (to check and update #context::distance and #context::st)
 */
void update_loc(context* ct){
	static uint32_t last_update = millis();
	const uint32_t timeout = millis() + GPS_SERIAL_TIMEOUT;

	while(millis() < timeout){
		if(Serial1.available() > 0 && gps.encode(Serial1.read())){
			if(ct->st.fetched){
				double lt = deg_to_rad(gps.location.lat());
				double ln = deg_to_rad(gps.location.lng());

				double phi[] = {ct->goals[ct->current_goal].lt, lt};
				double lambda[] = {ct->goals[ct->current_goal].ln, ln};

				double d_phi = phi[1] - phi[0];
				double d_lambda = lambda[1] - lambda[0];

				double a = sin(d_phi/2)*sin(d_phi/2) + sin(d_lambda/2)*sin(d_lambda/2) * cos(phi[0])*cos(phi[1]);
				double c = 2 * atan2(sqrt(a), sqrt(1-a));
				ct->distance = (int) round(c * EARTH_RADIUS);
				ct->st.reached = ct->distance <= DST_TOLERANCE;
			}

			if(gps.satellites.value() >= 3){
				ct->st.satcon = true;
				last_update = millis();
			}
			break;
		}
	}

	ct->st.satcon = millis() < (last_update + GPS_TIMEOUT);
}

/**
 * Iterates over every button in #context::btn and
 * updates their state if possible (see #BTN_DELAY and #button::event)
 *
 * @param ct (update #context::btn and #context:st)
 */
void update_buttons(context* ct){
	static uint32_t next_check = 0;
	const uint32_t current_time = millis();

	for(int i = 0; i < 3; i++){
		bool now_down = !digitalRead(ct->btn[i].pin);

		if(now_down != ct->btn[i].was_down){
			if(current_time > next_check){
				*ct->btn[i].event = now_down;
				ct->btn[i].was_down = now_down;
				next_check = current_time + BTN_DELAY;
			}else{
				*ct->btn[i].event = false;
			}
		}else{
			*ct->btn[i].event = false;
		}
	}
}

/**
 * Sets the 7-segment display array to the value stored in
 * #context::display_value.
 * Escape characters like #BLANK_C and #DASH_C will be parsed to their
 * coresponding segment value.
 *
 * @param ct (read #context::display_value)
 */
void update_display(context* ct){
	uint8_t digits[4] = {0};

	for(int i = 3; i >= 0; i--){
		if(ct->display_value[i] == BLANK_C){
			digits[i] = 0;
		}else if(ct->display_value[i] == DASH_C){
			digits[i] = SEG_G;
		}else{
			digits[i] = display.encodeDigit(ct->display_value[i]);
		}
	}

	display.setSegments(digits, 4, 0);
}


///////////////////////////// STATES //////////////////////////////

/**
 * This #handler is the initial state after setup is done and
 * is the quasi idle state (only changing when something happens)
 */
handler main_handler(context* ct){
	if(!ct->st.fetched){
		ct->display_value[0] = DASH_C;
		ct->display_value[1] = DASH_C;
		ct->display_value[2] = DASH_C;
		ct->display_value[3] = DASH_C;
	}else{
		set_distance(ct);
	}

	if(!ct->st.reset && ct->st.satcon && !ct->st.reached){
		return (handler) {main_handler};
	}else if(ct->st.reset){
		print_status("fetch_handler", ct);
		return (handler) {fetch_handler};
	}else if(ct->st.reached){
		ct->display_value[0] = DASH_C;
		ct->display_value[1] = BLANK_C;
		ct->display_value[2] = BLANK_C;
		ct->display_value[3] = BLANK_C;
		ct->input_pos = 0;

		print_status("subgoal_handler", ct);
		return (handler) {subgoal_handler};
	}else{
		print_status("satcon_handler", ct);
		return (handler) {satcon_handler};
	}
}

/**
 * When the location of #context::current_goal (see #goal)
 * is reached and no errors occured, this #handler checks
 * the entered password.
 */
handler subgoal_handler(context* ct){
	if(ct->st.pos){
		if(ct->display_value[ct->input_pos] == DASH_C){
			ct->display_value[ct->input_pos] = BLANK_C;
		}
		ct->input_pos = (ct->input_pos + 1) % 4;
		ct->display_value[ct->input_pos] = DASH_C;
	}
	if(ct->st.count){
		if(ct->display_value[ct->input_pos] == DASH_C){
			ct->display_value[ct->input_pos] = 0;
		}else{
			ct->display_value[ct->input_pos] = (ct->display_value[ct->input_pos] + 1) % 0x10;
		}
	}

	uint8_t match_c = 0;
	for(int i = 0; i < 4; i++){
		if(ct->display_value[i] == ct->goals[ct->current_goal].passwd[i]){
			++match_c;
		}
	}

	ct->st.passwd = match_c == 4;

	if(!ct->st.reset && !ct->st.passwd && ct->st.satcon && ct->st.reached){
		return (handler) {subgoal_handler};
	}else if(ct->st.reset){
		print_status("fetch_handler", ct);
		return (handler) {fetch_handler};
	}else if(ct->st.passwd){
		print_status("next_goal_handler", ct);
		return (handler) {next_goal_handler};
	}else if(!ct->st.satcon){
		print_status("satcon_handler", ct);
		return (handler) {satcon_handler};
	}else{
		print_status("main_handler", ct);
		return (handler) {main_handler};
	}
}

/**
 * In case the #RESET button was pressed and therefore
 * a #state::reset event occured, the fetch #handler
 * collects new goals from some source.
 * After goals have been collected, #context::current_goal and
 * #context::final_goal will be updated.
 */
handler fetch_handler(context* ct){
	ct->st.fetched = false;
	const uint32_t timeout = millis() + GPS_SERIAL_TIMEOUT;

	while(millis() < timeout){
		if(Serial1.available() > 0 && gps.encode(Serial1.read())){
			ct->goals[0].lt = deg_to_rad(gps.location.lat());
			ct->goals[0].ln = deg_to_rad(gps.location.lng());

			for(int i = 0; i < 4; i++){
				ct->goals[0].passwd[i] = i;
			}

			ct->current_goal = 0;
			ct->final_goal = 0;

			ct->st.fetched = true;
			break;
		}
	}

	if(ct->st.reset || !ct->st.fetched){
		return (handler) {fetch_handler};
	}else{
		print_status("main_handler", ct);
		return (handler) {main_handler};
	}
}

/**
 * This #handler checks whether a completed subgoal (#context::current_goal)
 * was the final one (#context::completed) and if so takes actions accordingly.
 */
handler next_goal_handler(context* ct){
	ct->st.completed = ct->current_goal == ct->final_goal;

	if(ct->st.completed){
		ct->display_value[0] = DASH_C;
		ct->display_value[1] = DASH_C;
		ct->display_value[2] = DASH_C;
		ct->display_value[3] = DASH_C;

		print_status("complete_handler", ct);
		return (handler) {complete_handler};
	}else{
		++ct->current_goal;
		print_status("main_handler", ct);
		return (handler) {main_handler};
	}
}

/**
 * After the last #goal is completed this #handler idles
 * until the #RESET button was pressed.
 */
handler complete_handler(context* ct){
	if(!ct->st.reset){
		return (handler) {complete_handler};
	}else{
		ct->st.completed = false;

		print_status("fetch_handler", ct);
		return (handler) {fetch_handler};
	}
}

/**
 * In case the #update_loc function couldn't get a sattelite connection
 * this #handler gives feedback via led (#LED_R) and tries to reconnect.
 */
handler satcon_handler(context* ct){
	digitalWrite(LED_R, HIGH);

	if(!ct->st.satcon){
		return (handler) {satcon_handler};
	}else{
		digitalWrite(LED_R, LOW);
		print_status("main_handler", ct);
		return (handler) {main_handler};
	}
}


////////////////////////////// SETUP //////////////////////////////

/** Global context that represents the state machine. */
context ct_g;
/** Gets executed and returns the next handler in #loop. */
handler handler_g;

void setup(){
	init_context(&ct_g);
	handler_g = (handler) {main_handler};
	pinMode(RESET, INPUT_PULLUP);
	pinMode(POS, INPUT_PULLUP);
	pinMode(COUNT, INPUT_PULLUP);
	pinMode(LED_R, OUTPUT);

	#ifndef NDEBUG
	Serial.begin(9600);
	while(!Serial);
	#endif

	Serial1.begin(9600);
	while(!Serial1);

	display.setBrightness(BRIGHTNESS);

	print_status("main_handler", &ct_g);
}


//////////////////////////// EXECUTION ////////////////////////////

void loop(){
	// input
	update_loc(&ct_g);
	update_buttons(&ct_g);

	// compute
	handler_g = handler_g.handle(&ct_g);

	// output
	update_display(&ct_g);
}
