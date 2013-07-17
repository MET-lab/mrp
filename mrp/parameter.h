/*
 *  parameter.h
 *  mrp
 *
 *  Created by Andrew McPherson on 10/19/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#ifndef PARAMETER_H
#define PARAMETER_H

#include <iostream>
#include <deque>
#include <cmath>
using namespace std;

// Set the number of samples between ramped parameter updates.  This is roughly equivalent
// to csound's ksmps variable.

#define PARAMETER_UPDATE_INTERVAL	16

enum
{
	shapeLinear = 0,
	shapeLogarithmic = 1,
	shapeStep = 2,
	shapeHold = 3
};

// Define a structure which, when cascaded in a vector,
// will let us define a time-varying sweep of a floating-point value
typedef struct
{
	double duration;	// How long (in seconds) until the next value is reached
	double nextValue;	// The next value of the parameter
	int shape;			// The shape of the curve to get from here to the next value
}
parameterValue;

typedef deque<parameterValue> timedParameter;

class Parameter
{
	friend ostream& operator<<(ostream& output, const Parameter& p);
public:
	Parameter(double initialValue, float sampleRate);			// Initialize with value
	double currentValue() { return val_; }						// Return the current parameter value
	void setCurrentValue(double newValue);						// Set the value (disables ramps in progress)
	void setRampValues(double startValue, timedParameter& rampValues);			
																// Set a new value and future ramp values 
	void appendRampValues(timedParameter& rampValues);			// Append ramp values to the current list
	bool ramp(int numSamples);									// Update the value; return true if it changed
	
private:
	void startRamp();			// Begin ramping to the next value
	
	double sampleRate_;			// Need to know the sample rate to properly calculate the step size
	double updateRate_;			// This value reflects the default parameter update interval (k-rate)
	double val_;				// The current value of the parameter
	double oldValue_;			// The value at the beginning of the current step
	double stepValue_;			// How much to step by each ramp() call
	int shape_;					// What type of operation to perform each step
	unsigned int sampleCount_;	// How many samples have gone by this ramp
	unsigned int sampleMax_;	// How many samples in this segment of the ramp
	timedParameter rampList_;	// The list of ramp changes to make
};

#endif // PARAMETER_H