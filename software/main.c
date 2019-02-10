// parking distance sensor
// for ATtiny84 at 8MHz

#define F_CPU 8000000UL

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdlib.h>
#include <stdint.h>

// Ultrasonic sensor module HC-SR04:
// - trigger at output pin PA0
// - echo signal at input pin PA1/PCINT1
#define SENSOR_TRIGGER_MASK (1<<PA0)  // trigger with edge to low
#define SENSOR_ECHO_IN_MASK (1<<PA1)  // high pulse length proportional to distance

// LEDs at output pins PA5, PA6 and PA7:
#define RED_MASK (1<<PA5)
#define GREEN_MASK (1<<PA6)
#define BLUE_MASK (1<<PA7)
#define HIGH_IS_ON 1        // 0 for low active, 1 for high active

typedef uint8_t bool;

void setLedsOnOff(bool red, bool green, bool blue)
{
	if (HIGH_IS_ON) {
		red = !red;
		green = !green;
		blue = !blue;
	}

	PORTA = (PORTA & ~(RED_MASK|GREEN_MASK|BLUE_MASK))
	| (red   ? 0 : RED_MASK)
	| (green ? 0 : GREEN_MASK)
	| (blue  ? 0 : BLUE_MASK);
}

void initLedsOnOff() {
	DDRA  |= RED_MASK | GREEN_MASK | BLUE_MASK;  // 1 = out
	setLedsOnOff(!HIGH_IS_ON, !HIGH_IS_ON, !HIGH_IS_ON);
}

void setLedsPwm(uint8_t red, uint8_t green, uint8_t blue) {

	if (HIGH_IS_ON) {
		red = 255 - red;
		green = 255 - green;
		blue = 255 - blue;
	}

	OCR1BH = 0;
	OCR1BL = red;
	OCR1AH = 0;
	OCR1AL = green;
	OCR0B = blue;
}

void initClockSource()
{
	// timer0 is used for both LED PWM and for measurement
	// of the ultrasonic sensor pulse length by counting overflows
	// => use no prescaling 
	// => 125ns clock cycle @ 8 MHz
	// => 32us for each 8-bit timer overflow
	TCCR0B = (TCCR0B & ~(1<<CS02 | 1<<CS02 | 1<<CS00)) | (1<<CS00);  // no prescaling timer0
	TCCR1B = (TCCR1B & ~(1<<CS12 | 1<<CS12 | 1<<CS10)) | (1<<CS10);  // no prescaling timer1 

	TIMSK0 |= (1<<TOIE0);       // 8-bit timer overflow interrupt timer0
}

void initLedsPwm() {
	DDRA |= (RED_MASK | GREEN_MASK | BLUE_MASK);

	TCCR0A = (1<<COM0B1) | (1<<COM0B0)   // inverting mode for 0A
	| (1<<WGM01) | (1<<WGM00);           // fast PWM mode for timer0
	TCCR0B = 0;
	
	TCCR1A = (1<<COM1A1) | (1<<COM1A0)   // inverting mode for 1A
	| (1<<COM1B1) | (1<<COM1B0)          // inverting mode for 1B
	| (1<<WGM10);                        // fast 8-bit PWM mode for timer1
	TCCR1B = (1<<WGM12);                 // fast 8-bit PWM mode for timer1
	TCCR1C = 0;
	
	initClockSource();                   // global timer prescaling
	
	sei();
}

void initSensor()
{
	DDRA |= SENSOR_TRIGGER_MASK;  // sensor trigger output
	PORTA |= SENSOR_TRIGGER_MASK; // prepare for first low-edge trigger
	// PORTA |= SENSOR_ECHO_IN_MASK; // pull-up for echo in
	
	GIMSK = (1<<PCIE0);      // echo signal interrupt
	PCMSK0 = (1<<PCINT1);    // at input PCINT1

	sei();
}

volatile int16_t lastCm = -1;
volatile uint16_t shownCount = 0;
#define MAX_SHOW_COUNT 600  // 60 seconds for 10 measurements per second
volatile uint8_t blink = 1;

void showDistance(int32_t clockCycles) {
	// duration is measured in clockCycles of 125ns
	// => 1 cm distance is about 472 clock cycles (depending on temperature)
	int16_t cm = clockCycles / 472;
	
	int16_t movementLimit = cm/2;
	if (movementLimit < 2)
		movementLimit = 2;
	if (movementLimit > 8)
		movementLimit = 10;
	
	if (abs(cm - lastCm) <= movementLimit) {
		// only minor movement/noise
		if (shownCount < MAX_SHOW_COUNT) {
			shownCount++;
		} else {
			// stop showing distance, only ready LED:
			setLedsPwm(0, 0, 1);
			return;
		}
	} else {
		shownCount = 0;
	}
	lastCm = cm;
	
	if (cm <= 6) {
		// red blinking
		if (blink) {
			setLedsPwm(255, 255, 255);
			blink = 0;
		} else {
			setLedsPwm(255, 0, 0);
			blink = 1;
		}
	} else {
		blink = 1;
		if (cm <= 12) {
			// red
			setLedsPwm(255, 0, 0);
		} else if (cm <= 26) {
			// yellow
			setLedsPwm(255, 100, 0);
		} else if (cm <= 80) {
			// green
			setLedsPwm(0, 220, 0);
		} else if (cm <= 150) {
			// green-blue
			setLedsPwm(0, 60, 60);
		}
		else {
			// almost off
			setLedsPwm(0, 0, 40);
		}
	}
}

int main(void)
{
	DDRA = DDRB = 0;    // 0 = all in
	PORTA = PORTB = 0;  // 0 = all pull-x off

	initLedsPwm();
	initSensor();

	while (1)
	{
		// 10 measurements per second when in showing state, 4 per second otherwise:
		if (shownCount <= MAX_SHOW_COUNT)
			_delay_ms(100);
		else
			_delay_ms(250);
		PORTA &= ~SENSOR_TRIGGER_MASK; // trigger measurement with edge to low
		_delay_us(25);
		PORTA |= SENSOR_TRIGGER_MASK;
	}
}

volatile bool waitingForEcho = 0;
volatile int32_t clockCycles;

ISR(TIM0_OVF_vect)
{
	if (waitingForEcho) {
		clockCycles += 256;
	}
}

ISR(PCINT0_vect)
{
	if (PINA & (1<<PINA1)) 
	{
		// pulse sent
		clockCycles = 0;
		waitingForEcho = 1;
		// might use clock register TCNT0 for more precision;
	} 
	else if (waitingForEcho)
	{
		// echo received
		waitingForEcho = 0;
		showDistance(clockCycles);
	}
}
