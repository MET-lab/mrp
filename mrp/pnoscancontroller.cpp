//
//  pnoscancontroller.cpp
//  mrp
//
//  Created by Jeff Gregorio on 9/7/12.
//
//

#include "pnoscancontroller.h"
#include <iomanip>
#include <fstream>

/*! ******************************************************************************************************************************************** //
 // ============================================================== PNOscanController ============================================================ //
 // ********************************************************************************************************************************************* */

// ============================================================================================================================================= //
// :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: Public Methods ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: //
// ============================================================================================================================================= //

// ============================================================================================================================================= //
// ----------------------------------------------------------- Constructor/Destructor ---------------------------------------------------------- //
// ============================================================================================================================================= //
PNOscanController::PNOscanController(MidiController *controller)
{
    midiController_ = controller;
    lastStateUpdate_ = currentTimeStamp_;
    reset(lastStateUpdate_);
    displayPNOstateChanges_ = true;
    stateMachineDebugMode_ = true;
    PNOverbose_ = false;
    currentTimeStamp_ = 0;
    
    cout << "*** PNOscanController " << endl;
}

PNOscanController::~PNOscanController()
{
    cout << "*** ~PNOscanController " << endl;
}

// ============================================================================================================================================= //
// -------------------------------------------------- Initialization and Data Member Assignment ------------------------------------------------ //
// ============================================================================================================================================= //
void PNOscanController::PNOscanInitialize(RtMidiOut midiOut, int mode, int hyst, int trigger, int release)
{
    //! Save these parameters as protected data members of MidiController as other processes may need them.
    PNOmode_ = mode;
    PNOhyst_ = hyst;
    PNOtrigger_ = trigger;
    PNOrelease_ = release;
    
	//! Make sure the PNOscan is in the correct mode and has the necessary trigger/release positions and hysteresis value.
	//! ****** Must set trigger, release, hysteresis BEFORE setting the mode ******
	if(trigger >= 0)
	{
		//! Construct sysex to change MIDI note on trigger position
		vector<unsigned char> v;
		v.push_back(0xF0);
		v.push_back(0x09);
		v.push_back(0x00);
		v.push_back(0x16);  // 0x16 = trigger position
		v.push_back(0x01);	// 1 means write
		for(int i=0; i < 6; i++) // set values
			v.push_back((unsigned char)trigger);
		v.push_back(0xF7);
		
        //        pthread_mutex_lock(&eventMutex_);
		cout << "Setting trigger position to " << trigger << endl;
		midiOut.sendMessage(&v);
        //        pthread_mutex_unlock(&eventMutex_);
	}
	
	if(release >= 0)
	{
		//! Construct sysex to change MIDI note off release position
		vector<unsigned char> v;
		v.push_back(0xF0);
		v.push_back(0x09);
		v.push_back(0x00);
		v.push_back(0x14);	// 0x14 = release position
		v.push_back(0x01);	// 1 means write
		for(int i=0; i < 6; i++) // set values
			v.push_back((unsigned char)release);
		v.push_back(0xF7);
		
        //        pthread_mutex_lock(&eventMutex_);
		cout << "Setting release position to " << release << endl;
		midiOut.sendMessage(&v);
        //        pthread_mutex_unlock(&eventMutex_);
	}
	
	if (hyst >= 0)
	{
		//! Construct sysex to change hysteresis value
		vector<unsigned char> v;
		v.push_back(0xF0);
		v.push_back(0x09);
		v.push_back(0x00);
		v.push_back(0x18);	// 0x18 = hysteresis
		v.push_back(0x01);	// 1 means write
		for(int i = 0; i < 6; i++)	// set values
			v.push_back((unsigned char)hyst);
		v.push_back(0xF7);
		
        //        pthread_mutex_lock(&eventMutex_);
		cout << "Setting hysteresis to " << hyst << endl;
		midiOut.sendMessage(&v);
        //        pthread_mutex_unlock(&eventMutex_);
	}
	
	if (mode >= 0)
	{
		/*! Modes:
		 *	0 -- MIDI
		 *	1 -- LoRes Diff (raw position, not scaled)
		 *   2 -- Scaled Diff (raw position, normalized)
		 */
		
		//! Construct sysex to change the mode
		vector<unsigned char> v;
		v.push_back(0xF0);
		v.push_back(0x09);
		v.push_back(0x00);
		//v.push_back(0x21);
		v.push_back((unsigned char)mode);
		v.push_back(0xF7);
		
        //        pthread_mutex_lock(&eventMutex_);
		cout << "Setting mode to " << mode << endl;
		midiOut.sendMessage(&v);
        //        pthread_mutex_unlock(&eventMutex_);
	}
}

// ============================================================================================================================================= //
// ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: Protected Methods ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: //
// ============================================================================================================================================= //
int PNOscanController::whiteKeyAbove(unsigned int key)
{
    if(key >= 87) return 0;
    if(kPianoBarKeyColor[key + 1] == K_B) { return key + 2; }
    else { return key + 1; }
}

int PNOscanController::whiteKeyBelow(unsigned int key)
{
    if(key <= 0) return 87;
    if(kPianoBarKeyColor[key - 1] == K_B) { return key - 2; }
    else { return key - 1; }
}

RealTimeMidiNote *PNOscanController::getNoteForKey(unsigned int key)
{
    int midiNote = key + 21;
    
    //! MidiController's Note map key, not the piano key
//    unsigned int noteMapKey = ((unsigned int)midiChannel_ << 8) + (unsigned int)midiNote;
    unsigned int noteMapKey = ((unsigned int)0 << 8) + (unsigned int)midiNote;
    
    if (midiController_->currentNotes_.count(noteMapKey) == 0)
    {
        cerr << "Error finding note for MIDI note " << midiNote << " (key = " << key << ") on channel " << midiChannel_ << endl;
        return NULL;
    }
    
    Note *note = midiController_->currentNotes_[noteMapKey];
    
    if (typeid(*note) != typeid(RealTimeMidiNote))
    {
        cerr << "Note for MIDI note " << midiNote << " (key = " << key << ") on channel " << midiChannel_ << " is not of type RealTimeMidiNote" << endl;
        return NULL;
    }
    else return (RealTimeMidiNote *)note;
}

// ============================================================================================================================================= //
// :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: Private Methods :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: //
// ============================================================================================================================================= //
void PNOscanController::reset(ps_timestamp cts)
{
    //! Set current and previous states to Idle
    vector<int> v;
    v.push_back(PA_kKeyStateIdle);
    v.push_back(PA_kKeyStateIdle);
    
    //! Standard 88 key piano keyboard
    for (int i = 0; i <= 87; ++i)
    {
        keyStates_[i] = v;
        lastNoteOffTime_[i] = cts;
        lastNoteOnTime_[i] = cts;
        lastMidiNoteOnVelocities_[i] = 0;
    }
    v.clear();
    return;
}
// ============================================================================================================================================= //
// ------------------------------------------------------------ MIDI Event Methods ------------------------------------------------------------- //
// ============================================================================================================================================= //
void PNOscanController::noteOn(vector<unsigned char> *message)
{
//    cout << "PNOscanController::noteOn()" << endl;
	int midiNote = (*message)[1];
    int midiVelocity = (*message)[2];
    unsigned int key = midiNote - 21;
    
    //! Keep track of a currently sounding note (as far as PNOscanController is concerned) until a NOTEOFF message is received for this key
    currentMidiEvents_[key] = true;
    
    //! lastNoteOnTime_ and lastMidiNoteOnVelocities_ are necessary for the key state machine
    lastNoteOnTime_[key] = currentTimeStamp_;
    lastMidiNoteOnVelocities_[key] = midiVelocity;
    
	return;
}

void PNOscanController::noteOff(vector<unsigned char> *message)
{
//    cout << "PNOscanController::noteOff()" << endl;
	int midiNote = (*message)[1];
    unsigned int key = midiNote - 21;
    PAevent *paEvent;
    
    //! Record the timestamp of the NOTEOFF event
    lastNoteOffTime_[key] = currentTimeStamp_;
    
    //! Keep track of currently sounding notes
    currentMidiEvents_[key] = false;
    
    //! Check for current PAevents
    if (currentPAevents_.count(key) != 0)
    {
        paEvent = currentPAevents_[key];
        
        //! Change the key's state to idle
        changeKeyState(key, PA_kKeyStateIdle, currentTimeStamp_);
        
        //! In debug mode, print its motion and state histories to the terminal
        if (stateMachineDebugMode_)
        {
            currentPAevents_[key]->printKeyMotionHistories();
        }
        //! Erase the PAevent from the map
        currentPAevents_.erase(key);
        
        //! Delete the actual object
        delete paEvent;
    }
	return;
}
void PNOscanController::handlePolyphonicAftertouch(vector<unsigned char> *message)
{
    int midiNote = (*message)[1];
    unsigned int key = midiNote - 21;
    int position = (*message)[2];
    
    RealTimeMidiNote *note = getNoteForKey(key);
    
    //! Check to see if the Note object exists
    if (note != NULL)
    {
        //! Check to see if a PAevent has been created for the current key
        if (currentPAevents_.count(key) == 0)
        {
            cout << "No PAevent yet exists for piano key " << key << endl;
            //! Create a PAevent for the piano key
            currentPAevents_[key] = new PAevent(this, note, message, currentTimeStamp_);
        }
        //! Check immedaitely after creating a PAevent (rather than an if-else, which would update the position history after another PA message).
        if(currentPAevents_.count(key) > 0)
        {
            //! Tell the note object to begin
//            currentPAevents_[key]->getNote()->begin(midiController_->pianoDamperLifted(key));
            
            //! If the PAevent already exists, update its position history
            currentPAevents_[key]->updateKeyPositionHistory(position, currentTimeStamp_);
            
            //! Perform motion analysis for new position data and update key states accordingly
            currentPAevents_[key]->runningMotionAnalysis();
            currentPAevents_[key]->updateKeyStates();
            currentPAevents_[key]->sendKeyStateMessages();
        }
    }
}

void PNOscanController::changeKeyState(unsigned int key, int newState, ps_timestamp timeStamp)
{
    //! Shift old "current" state to previous position, update current state.
    keyStates_[key][1] = keyStates_[key][0];
    keyStates_[key][0] = newState;
    
    //! Keep track of the timestamp when the state changes.
    lastStateUpdate_ = timeStamp;
    
    //! Keep track of the timestamp of Down states for the multi-key gesture handlers
    if (newState == PA_kKeyStateDown)   lastDownStateTime_[key] = timeStamp;
    
    if (displayPNOstateChanges_)
    {
        string stateString = kKeyStateToString(newState);
        cout << "\n******** Key state for key " << key << " changed to \"" << stateString << "\"\n\n";
    }
}

void PNOscanController::printAllPAevents()
{
    map<unsigned int, PAevent*>::iterator itr;
    
    //! Check for Note objects
    if (currentPAevents_.size() == 0)
    {
        cout << "No PAevents currently exist.\n";
    }
    else
    {
        for (itr = currentPAevents_.begin(); itr != currentPAevents_.end(); ++itr)
        {
            cout << "PAevent exists for key number " << (*itr).first << endl;
        }
    }
}

string PNOscanController::kKeyStateToString(int state)
{
    string stateString;
    
    switch (state)
    {
        case PA_kKeyStateIdle:
            stateString = "Idle";
            break;
        case PA_kKeyStatePretouch:
            stateString = "Pretouch";
            break;
        case PA_kKeyStatePreVibrato:
            stateString = "PreVibrato";
            break;
        case PA_kKeyStateTap:
            stateString = "Tap";
            break;
        case PA_kKeyStatePress:
            stateString = "Press";
            break;
        case PA_kKeyStateDown:
            stateString = "Down";
            break;
        case PA_kKeyStateAftertouch:
            stateString = "Aftertouch";
            break;
        case PA_kKeyStateAfterVibrato:
            stateString = "AfterVibrato";
            break;
        case PA_kKeyStateRelease:
            stateString = "Release";
            break;
        case PA_kKeyStateDisabled:
            stateString = "Disabled";
            break;
        default:
            break;
    }
    return stateString;
}


#pragma mark class PAevent
// ********************************************************************************************************************************************* //
// ================================================================ PAevent ==================================================================== //
// ********************************************************************************************************************************************* //

// ============================================================================================================================================= //
// ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: Public Methods :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: //
// ============================================================================================================================================= //

// ============================================================================================================================================= //
// --------------------------------------------------------- Constructor/Destructor ------------------------------------------------------------ //
// ============================================================================================================================================= //
PNOscanController::PAevent::PAevent(PNOscanController *PNOcontroller, RealTimeMidiNote *note, vector<unsigned char> *message, ps_timestamp currentTimestamp)
{
    PNOcontroller_ = PNOcontroller;
    midiController_ = PNOcontroller_->midiController_;
//    midiChannel_ = ((*message)[0] & 0x0F);
    midiNoteNumber_ = (*message)[1];
    
    noteFreq_ = midiController_->midiNoteToFrequency(midiNoteNumber_);
    
    if(typeid(*note) == typeid(RealTimeMidiNote))   note_ = (RealTimeMidiNote*)note;
    else    cerr << "Note for PAevent is not of type RealTimeMidiNote." << endl;
    
	key_ = midiNoteNumber_ - 21;                                //! Piano key number (0-87)
    startTime_ = PNOcontroller->lastNoteOnTime_[key_];
    midiVelocity_ = PNOcontroller->lastMidiNoteOnVelocities_[key_];
    int initialPosition = 8; // controller->getPNOscanTriggerPosition();
    
    //! Initialize the motion histories and instantaneous averages
    avgKeyPosition_ = keyPositionHistory_[startTime_] = initialPosition;
    avgKeyVelocity_ = keyVelocityHistory_[startTime_] = midiVelocity_;
    peakKeyAcceleration_ = avgKeyAcceleration_ = keyAccelerationHistory_[startTime_] = 0;
    
    //! If we're in debug mode, save all averages and peaks in a map by timestamp for analysis
    if (PNOcontroller->getDebugModeFlag())
    {
        avgKeyPositionHistory_[startTime_] = avgKeyPosition_;
        avgKeyVelocityHistory_[startTime_] = avgKeyVelocity_;
        avgKeyAccelerationHistory_[startTime_] = avgKeyAcceleration_;
        peakKeyAccelerationHistory_[startTime_] = peakKeyAcceleration_;
    }
    
    //! Initialize the key state to idle
    //    controller_->changeKeyState(key_, PA_kKeyStateIdle, startTime_);
    
    //! Initialize the vibrato counter
    //    keyVibratoCount_[startTime_] = 0;
	
	cout << "*** PAevent() for key " << key_ << endl;
}

PNOscanController::PAevent::~PAevent()
{
	cout << "*** ~PAevent() for key " << key_ << endl;
}


// =========================
#pragma mark Motion Analysis
// =========================
void PNOscanController::PAevent::runningMotionAnalysis()
{
    //! Current and previous position, velocity, acceleration, and timestamp
    int p0, p1;
    double v0, v1;
    double a0, a1;
    ps_timestamp t0, t1;
    
    //! Find last position and time of the position, velocity, and acceleration histories
	map<ps_timestamp, unsigned int>::iterator pos_itr = keyPositionHistory_.end();
    map<ps_timestamp, double>::iterator vel_itr = keyVelocityHistory_.end();
    map<ps_timestamp, double>::iterator acc_itr = keyAccelerationHistory_.end();
    
    /*! The map member fucntion end() returns ONE PAST the end of a map, so we need to decrement the iterator.  However,
     PNOscanController::handlePolyAftertouch() calls updatePositionHistory() just before it calls runningMotionAnalysis(), therefore
     keyPositionHistory already contains the position for the current timestamp, therefore is one data point longer than
     keyVelocityHistory and keyAccelerationHistory, so we decrement its iterator to get to the current value and we decrement the
     other iterators to get the previous values. */
    pos_itr--;
    
    //! Current position and time
    p1 = (*pos_itr).second;
    t1 = (*pos_itr).first;
    
    pos_itr--;
    vel_itr--;
    acc_itr--;
    
    //! Previous position, velocity, acceleration, and time
    p0 = (*pos_itr).second;
    v0 = (*vel_itr).second;
    a0 = (*acc_itr).second;
    t0 = (*pos_itr).first;
    
    //! Calculate current velcity and store it in the velocity history map
    v1 = (p1-p0)/(t1-t0);
    keyVelocityHistory_[t1] = v1;
    
    //! Calculate the current acceleration and store it in the acceleration history map
    a1 = (v1-v0)/(t1-t0);
    keyAccelerationHistory_[t1] = a1;
    
    /*! Note: This is kind of a hack, but first velocity and acceleration points tend to be inflated, so we'll divide them by a scalar. */
    if (keyPositionHistory_.size() < 2)
    {
        v1 /= 3;
        a1 /= 3;
    }
    
    //! Update running averages
    avgKeyPosition_ = updateRunningAverage(avgKeyPosition_, p0, p1, t0, t1);
    avgKeyVelocity_ = updateRunningAverage(avgKeyVelocity_, v0, v1, t0, t1);
    avgKeyAcceleration_ = updateRunningAverage(avgKeyAcceleration_, a0, a1, t0, t1);
    
    //! Update peakAcceleration_
    if (a1 > peakKeyAcceleration_)    { peakKeyAcceleration_ = a1; }
    
    
    if (PNOcontroller_->getDebugModeFlag())
    {
        //! Update running averages
        avgKeyPositionHistory_[t1] = avgKeyPosition_;
        avgKeyVelocityHistory_[t1] = avgKeyVelocity_;
        avgKeyAccelerationHistory_[t1] = avgKeyAcceleration_;
        
        //! Update peakAcceleration_
        peakKeyAccelerationHistory_[t1] = peakKeyAcceleration_;
    }
}

double PNOscanController::PAevent::updateRunningAverage(double previousAverage, double x0, double x1, ps_timestamp t0, ps_timestamp t1)
{
    double old = previousAverage;
    double oldWeight = t0/t1;
    double update = x0 + ((x1-x0)/(t1-t0))/2;
    double updateWeight = 1-oldWeight;
    
    double new_ = old*oldWeight + update*updateWeight;
    return new_;
}

void PNOscanController::PAevent::updateKeyStates()
{
    /*! Allowable state transitions:
     Idle         --> Pretouch, Press, Tap
     Pretouch     --> Idle, Press, Tap, PreVibrato
     Tap          --> Idle, Press
     PreVibrato   --> Idle, Pretouch, Press
     Press        --> Down, Pretouch, Release
     Down         --> Release, Aftertouch
     Aftertouch   --> Down, AfterVibrato
     AfterVibrato --> Down, Release
     Release      --> Idle, Press
     */
    
    //! Initialize iterators to end (most recent updates) of each map containing key gesture features
    map<ps_timestamp, unsigned int>::const_iterator pos_itr = keyPositionHistory_.end();
    map<ps_timestamp, double>::const_iterator vel_itr = keyVelocityHistory_.end();
    map<ps_timestamp, double>::const_iterator acc_itr = keyAccelerationHistory_.end();
    
    //! Can't forget that map iterator member end() returns ONE PAST the last element
    pos_itr--;
    vel_itr--;
    acc_itr--;
    
    unsigned int position     = (*pos_itr).second;
    double       velocity     = (*vel_itr).second;
    //    double       acceleration = (*acc_itr).second;
    
    ps_timestamp lastVibratoTimestamp;
    
    //! Initialize the iterator to the end of the vibrato counter if it exists
    if (!keyVibratoCount_.empty())
    {
        map<ps_timestamp, int>::iterator vib_itr = keyVibratoCount_.end();
        vib_itr--;
        lastVibratoTimestamp = (*vib_itr).first;
    }
    
    //! Get the current and previous state from the PNOscanController that created the PAevent
    vector<int> v;
    v = PNOcontroller_->getKeyState(key_);
    
    int currentState  = v[0];
    //    int previousState = v[1];
    
    ps_timestamp cts = PNOcontroller_->currentTimeStamp_;
    
    /*********************************** Verbose State Machine *************************************/
    if (PNOcontroller_->getVerboseModeFlag() || PNOcontroller_->getDebugModeFlag())
    {
        //! For displaying state information on the terminal
        string currentStateString = PNOcontroller_->kKeyStateToString(currentState);
        keyStateChangeHistory_[cts] = currentStateString;
        
        if (PNOcontroller_->getVerboseModeFlag())  printCurrentMotionFeatures();
    }
    /***********************************************************************************************/
    
    // ===========================
#pragma mark Key State Machine
    // ===========================
    switch (currentState)
    {
        case PA_kKeyStateIdle:
            //! Test for the current position less than the idle threshold
            if (position < PA_keyIdlePositionThreshold) break;
            
            //! Test for peak acceleration greater than the Tap threshold
            if (peakKeyAcceleration_ > PA_keyTapAccelerationThreshold)
            {
                PNOcontroller_->changeKeyState(key_, PA_kKeyStateTap, cts);
                break;
            }
            
            /*! If we're above the Idle position threshold and below the Tap acceleration threshold, we're either going to transition to
             Pretouch for a low average velocity or directly to Press for a higher average velocity. */
            if (velocity > PA_keyIdlePressVelocityThreshold)
            {
                PNOcontroller_->changeKeyState(key_, PA_kKeyStatePress, cts);
                break;
            }
            else
            {
                PNOcontroller_->changeKeyState(key_, PA_kKeyStatePretouch, cts);
                keyPreVibratoFlag_ = true;
            }
            break;
            
        case PA_kKeyStatePretouch:
        case PA_kKeyStateTap:
            //! Test position for return to Idle
            if (position < PA_keyIdlePositionThreshold)
            {
                PNOcontroller_->changeKeyState(key_, PA_kKeyStateIdle, cts);
                break;
            }
            
            /*! Transition to Press above the Press position threshold.  Also testing for positive velocity prevents transitioning back
             and forth between Pretouch and Press if we've entered Pretouch from a Press state (in which case the velocity would be
             negative), as well as allowing low and negative velocity movements below the normal Press position when attempting the
             previbrato gesture. */
            if (position > PA_keyPressPositionThreshold && velocity > PA_keyPretouchPressVelocityThreshold)
            {
                PNOcontroller_->changeKeyState(key_, PA_kKeyStatePress, cts);
                break;
            }
            
            //! We still may want to transition to a Tap
            if (peakKeyAcceleration_ >= PA_keyTapAccelerationThreshold)
            {
                PNOcontroller_->changeKeyState(key_, PA_kKeyStateTap, cts);
                break;
            }
            
            /*! Increment the vibrato counter if the current velocity has reversed direction compared to the previous velocity. */
            if ((keyPreVibratoFlag_ && velocity < 0) || (!keyPreVibratoFlag_ && velocity > 0))
            {
                /*! Give unlimited time from entering a Pretouch state to begin the vibrato gesture, but give limited time to complete
                 the gesture.  */
                if (keyVibratoCount_.empty())
                {
                    //! Next time around lastVibratoTimestamp will pull this value of cts
                    keyVibratoCount_[cts] = 1;
                    break;
                }
                else if (cts - lastVibratoTimestamp < PA_keyVibratoMaxFrameSpacing)
                {
                    keyVibratoCount_[cts] = keyVibratoCount_[lastVibratoTimestamp] + 1;
                    //! Invert the flag so we can look for a velocity in the opposite direction next time
                    keyAfterVibratoFlag_ = !keyAfterVibratoFlag_;
                    
                    //! Check the counter immediately after an update and change to pre vibrato when the counter excdeeds the threshold
                    if (keyVibratoCount_[cts] > PA_keyVibratoCounterThreshold)
                    {
                        PNOcontroller_->changeKeyState(key_, PA_kKeyStatePreVibrato, cts);
                        break;
                    }
                }
            }
            break;
            
//        case PA_kKeyStateTap:
//            /* Note:  Andrew's state machine goes immediately from Tap to Pretouch. */
//            //            //! For now, go immediately to Pretouch
//            //            controller_->changeKeyState(key_, PA_kKeyStatePretouch, cts);
//            
//            //! Check position for a transition to idle
//            if (position < PA_keyIdlePositionThreshold)
//            {
//                PNOcontroller_->changeKeyState(key_, PA_kKeyStateIdle, cts);
//                break;
//            }
//            
//            //! Check position for a transition to press
//            if (position > PA_keyPressPositionThreshold)
//            {
//                PNOcontroller_->changeKeyState(key_, PA_kKeyStatePress, cts);
//                break;
//            }
//            break;
            
        case PA_kKeyStatePreVibrato:
            //! Test avg position and avg velocity for idle thresholds
            if (position < PA_keyIdlePositionThreshold)
            {
                PNOcontroller_->changeKeyState(key_, PA_kKeyStateIdle, cts);
                //! Reset the vibrato counter
                keyVibratoCount_.clear();
                break;
            }
            
            /*! Note:  We use average position rather than position  for looser restrictions on remaining in a PreVibrato state. */
            //! Keep an eye out for a key press
            if (avgKeyPosition_ > PA_keyPreVibratoPressPositionThreshold && velocity > 0)
            {
                PNOcontroller_->changeKeyState(key_, PA_kKeyStatePress, cts);
                //! Reset the vibrato counter
                keyVibratoCount_.clear();
                break;
            }
            
            /*! Keep incrementing the vibrato counter */
            if ((keyPreVibratoFlag_ && velocity < 0) || (!keyPreVibratoFlag_ && velocity > 0))
            {
                if (cts - lastVibratoTimestamp < PA_keyVibratoMaxFrameSpacing)
                {
                    keyVibratoCount_[cts] = keyVibratoCount_[lastVibratoTimestamp] + 1;
                    //! Invert the flag so we can look for a velocity in the opposite direction next time
                    keyAfterVibratoFlag_ = !keyAfterVibratoFlag_;
                }
            }
            
            //! Check for vibrato timeout
            /*! Note:  this will remain in AfterVibrato if we hold the key in a down state after triggering AfterVibrato because the PNOscan
             will not send a position update until the position changes.  A possible solution may be to use sensor noise to obtain
             a "regular" stream of position data.  Remaining in AfterVibrato until release may be acceptable, however. */
            if (cts - lastVibratoTimestamp > PA_keyVibratoTimeout)
            {
                PNOcontroller_->changeKeyState(key_, PA_kKeyStatePretouch, cts);
                keyVibratoCount_.clear();
                break;
            }
            break;
            
        case PA_kKeyStatePress:
            
            //! Check the current position for down state
            if (position >= PA_keyDownPositionThreshold)
            {
                PNOcontroller_->changeKeyState(key_, PA_kKeyStateDown, cts);
                /*! Entering Down from Press means we enter with positive velocity, so we set the flag to look for negative velocity
                 to increment the vibrato counter. */
                keyAfterVibratoFlag_ = true;
                break;
            }
            
            //! Check for velocity reversal to trigger a release
            if (velocity < -200)
            {
                if (abs(velocity) > PA_keyPressReleaseVelocityThreshold)
                {
                    PNOcontroller_->changeKeyState(key_, PA_kKeyStateRelease, cts);
                    break;
                }
                else
                {
                    PNOcontroller_->changeKeyState(key_, PA_kKeyStatePretouch, cts);
                    /*! Entering pretouch from Press means we enter with negative velocity, so we set the flag to look for a positive
                     velocity to increment the vibrato counter. */
                    keyPreVibratoFlag_ = false;
                    break;
                }
            }
            break;
            
        case PA_kKeyStateDown:
            
            // Check the current position for release
            if (position < PA_keyReleasePositionThreshold)
            {
                PNOcontroller_->changeKeyState(key_, PA_kKeyStateRelease, cts);
                break;
            }
            
            // Increment the vibrato counter if the current velocity has reversed direction compared to the previous velocity
            if ((keyAfterVibratoFlag_ && velocity < 0) || (!keyAfterVibratoFlag_ && velocity > 0))
            {
                /*! Give unlimited time from entering a Down state to begin the vibrato gesture, but give limited time to complete the
                 vibrato gesture.  */
                if (keyVibratoCount_.empty())
                {
                    //! Next time around lastVibratoTimestamp will pull this value of cts
                    keyVibratoCount_[cts] = 1;
                }
                else if (cts - lastVibratoTimestamp < PA_keyVibratoMaxFrameSpacing)
                {
                    keyVibratoCount_[cts] = keyVibratoCount_[lastVibratoTimestamp] + 1;
                    //! Invert the flag so we can look for a velocity in the opposite direction next time
                    keyAfterVibratoFlag_ = !keyAfterVibratoFlag_;
                    
                    //! Check the counter immediately after an update and change to pre vibrato when the counter exceeds the threshold
                    if (keyVibratoCount_[cts] > PA_keyVibratoCounterThreshold)
                    {
                        PNOcontroller_->changeKeyState(key_, PA_kKeyStateAfterVibrato, cts);
                        break;
                    }
                }
            }
            
            // Check for a transition to aftertouch
            if (position > PA_keyAftertouchPositionThreshold)
            {
                PNOcontroller_->changeKeyState(key_, PA_kKeyStateAftertouch, cts);
                break;
            }
            
            break;
            
        case PA_kKeyStateAftertouch:
            
            // Allow incrementing the vibrato counter in both Down and Aftertouch states
            // Increment the vibrato counter if the current velocity has reversed direction compared to the previous velocity
            if ((keyAfterVibratoFlag_ && velocity < 0) || (!keyAfterVibratoFlag_ && velocity > 0))
            {
                /*! Give unlimited time from entering a Down state to begin the vibrato gesture, but give limited time to complete the
                 vibrato gesture.  */
                if (keyVibratoCount_.empty())
                {
                    //! Next time around lastVibratoTimestamp will pull this value of cts
                    keyVibratoCount_[cts] = 1;
                }
                else if (cts - lastVibratoTimestamp < PA_keyVibratoMaxFrameSpacing)
                {
                    keyVibratoCount_[cts] = keyVibratoCount_[lastVibratoTimestamp] + 1;
                    //! Invert the flag so we can look for a velocity in the opposite direction next time
                    keyAfterVibratoFlag_ = !keyAfterVibratoFlag_;
                    
                    //! Check the counter immediately after an update and change to pre vibrato when the counter exceeds the threshold
                    if (keyVibratoCount_[cts] > PA_keyVibratoCounterThreshold)
                    {
                        PNOcontroller_->changeKeyState(key_, PA_kKeyStateAfterVibrato, cts);
                        break;
                    }
                }
            }
            
            // Check for transition back to Down
            if (position < PA_keyAftertouchPositionThreshold)
            {
                PNOcontroller_->changeKeyState(key_, PA_kKeyStateDown, cts);
                break;
            }
            
            break;
            
        case PA_kKeyStateAfterVibrato:
            //! Check the current position for a release
            if (position < PA_keyAfterVibratoReleasePositionThreshold)
            {
                //! Change to Release and reset the vibrato counter
                PNOcontroller_->changeKeyState(key_, PA_kKeyStateRelease, cts);
                keyVibratoCount_.clear();
                break;
            }
            
            /*! Keep incrementing the vibrato counter */
            if ((keyAfterVibratoFlag_ && velocity < 0) || (!keyAfterVibratoFlag_ && velocity > 0))
            {
                if (cts - lastVibratoTimestamp < PA_keyVibratoMaxFrameSpacing)
                {
                    keyVibratoCount_[cts] = keyVibratoCount_[lastVibratoTimestamp] + 1;
                    //! Invert the flag so we can look for a velocity in the opposite direction next time
                    keyAfterVibratoFlag_ = !keyAfterVibratoFlag_;
                }
            }
            
            //! Check for vibrato timeout
            /*! Note:  this will remain in AfterVibrato if we hold the key in a down state after triggering AfterVibrato because the PNOscan
             will not send a position update until the position changes.  A possible solution may be to use sensor noise to obtain
             a "regular" stream of position data.  Remaining in AfterVibrato until release may be acceptable, however. */
            if (cts - lastVibratoTimestamp > PA_keyVibratoTimeout)
            {
                PNOcontroller_->changeKeyState(key_, PA_kKeyStateDown, cts);
                keyVibratoCount_.clear();
                break;
            }
            break;
            
        case PA_kKeyStateRelease:
            //! Check the current position for an idle state
            if (position < PA_keyIdlePositionThreshold)
            {
                PNOcontroller_->changeKeyState(key_, PA_kKeyStateIdle, cts);
                break;
            }
            
            //! Check velocity reversal to retrigger a Press
            if (velocity > 0)
            {
                if (position > PA_keyPressPositionThreshold)
                {
                    PNOcontroller_->changeKeyState(key_, PA_kKeyStatePress, cts);
                    break;
                }
                else
                {
                    PNOcontroller_->changeKeyState(key_, PA_kKeyStatePretouch, cts);
                    break;
                }
            }
            break;
            
        default:
            cerr << "Warning:  Key " << key_ << " is in an unknown state.\n";
            PNOcontroller_->changeKeyState(key_, PA_kKeyStateIdle, cts);
            break;
            
    }
}

/** Called whenever a key is in Pretouch or Press. Looks for neighboring keys already in the Down state. The Down key will be the
    'center' key and this key will be the 'auxiliary' key whose position we use to bend the center note **/
bool PNOscanController::PAevent::handleMultiKeyPitchBend()
{    
    bool rv;
    bool pitchBendUp = false;
    bool pitchBendDn = false;
    
    // This note (in Pretouch or Press) will be the auxiliary note whose position we
    RealTimeMidiNote *centerNote;
    RealTimeMidiNote *auxNote;
    
    double cntNotePitch;

    // Query the states of one and two keys above the pretouch key
    vector<int> oneAboveStates = PNOcontroller_->getKeyState(key_ + 1);
    vector<int> twoAboveStates = PNOcontroller_->getKeyState(key_ + 2);
    
    // If the first key above is in a "down-ish" state, use it as the center note to bend *DOWN*
    if( oneAboveStates[0] == PA_kKeyStateDown || oneAboveStates[0] == PA_kKeyStateAftertouch || oneAboveStates[0] == PA_kKeyStateAfterVibrato ) {
        
        if ( PNOcontroller_->lastDownStateTime_[key_ + 1] < startTime_ ) {
            
            pitchBendDn = true;
            
            centerNote = PNOcontroller_->getNoteForKey(key_ + 1);
            cntNotePitch = midiController_->midiNoteToFrequency(key_+21 + 1);
            auxNote = note_;
        }
    }
    // Otherwise, look for two keys above
    else if ( twoAboveStates[0] == PA_kKeyStateDown || twoAboveStates[0] == PA_kKeyStateAftertouch || twoAboveStates[0] == PA_kKeyStateAfterVibrato ) {
        
        if ( PNOcontroller_->lastDownStateTime_[key_ + 2] < startTime_ ) {
            
            pitchBendDn = true;
            
            centerNote = PNOcontroller_->getNoteForKey(key_ + 2);
            cntNotePitch = midiController_->midiNoteToFrequency(key_+21 + 2);
            auxNote = note_;
        }
    }
    
    // ==================================== Pitch Bend Down ====================================
    // If the center key is above the Pretouch key, then the Pretouch key initiates a bend DOWN
    if( pitchBendDn ) {
        
        rv = true;
        
        //! getNoteForKey() returns NULL if (typeid(note) != typeid(RealTimeMidiNote))
        if (centerNote != NULL && auxNote != NULL)
        {
            // Get current position of aux key
            map<ps_timestamp, unsigned int>::iterator pos_itr = keyPositionHistory_.end();
            pos_itr--;
            
            //! Get pitch information to scale pitch bends linearly between two pitches
            double auxNotePitch  = noteFreq_;
            double pitchDiff     = cntNotePitch - auxNotePitch;
            
            //! Frequency per division
            double decrement        = pitchDiff / (PA_keyDownPositionThreshold - PA_keyIdlePositionThreshold);
            double position         = (double)(*pos_itr).second;
            
            //! Updated (bent) pitch based on aux key position
            double targetBentPitch  = cntNotePitch - decrement*(position - PA_keyIdlePositionThreshold);
            
#ifdef DEBUG_SYNTH_PARAMETERS
            cout << "************************** Pitch Bend Down ***************************"<< endl;
            cout << "Center Note Pitch      = " << cntNotePitch         << "Hz"             << endl;
            cout << "Aux Note Pitch         = " << auxNotePitch         << "Hz"             << endl;
            cout << "Pitch Difference       = " << pitchDiff            << "Hz"             << endl;
            cout << "Decrement              = " << decrement            << "Hz/pos.unit"    << endl;
            cout << "Aux Key Position       = " << position                                 << endl;
            cout << "Target Bent Pitch      = " << targetBentPitch      << "Hz"             << endl;
            cout << "**********************************************************************"<< endl;
#endif
            cout << "*** Updating center note parameters ***" << endl;
            centerNote->setAbsolutePitch(cntNotePitch, targetBentPitch, true);
            centerNote->updateSynthParameters();
            
            cout << "*** Updating auxiliary note parameters ***" << endl;
            auxNote->setAbsolutePitch(auxNotePitch, targetBentPitch, true);
            auxNote->updateSynthParameters();
        }
    }
    
    // Query the states of one and two keys below the pretouch key
    vector<int> oneBelowStates = PNOcontroller_->getKeyState(key_ - 1);
    vector<int> twoBelowStates = PNOcontroller_->getKeyState(key_ - 2);
    
    // If the first key below is in a "down-ish" state, use it as the center note to bend *UP*
    if( oneBelowStates[0] == PA_kKeyStateDown || oneBelowStates[0] == PA_kKeyStateAftertouch || oneBelowStates[0] == PA_kKeyStateAfterVibrato ) {
        
        if ( PNOcontroller_->lastDownStateTime_[key_ - 1] < startTime_ ) {
            
            pitchBendUp = true;
            
            centerNote = PNOcontroller_->getNoteForKey(key_ - 1);
            cntNotePitch = midiController_->midiNoteToFrequency(key_+21 - 1);
            auxNote = note_;
        }
    }
    // Otherwise, look for two keys below
    else if ( twoBelowStates[0] == PA_kKeyStateDown || twoBelowStates[0] == PA_kKeyStateAftertouch || twoBelowStates[0] == PA_kKeyStateAfterVibrato ) {
        
        if ( PNOcontroller_->lastDownStateTime_[key_ - 2] < startTime_ ) {
            
            pitchBendUp = true;
            
            centerNote = PNOcontroller_->getNoteForKey(key_ - 2);
            cntNotePitch = midiController_->midiNoteToFrequency(key_+21 - 2);
            auxNote = note_;
        }
    }
    
    // ========================================= Pitch Bend Up =========================================
    // Otherwise, if the center key is below the Pretouch key, then the Pretouch key initiates a bend UP
    if( pitchBendUp ) {
        
        rv = true;
        
        //! getNoteForKey() returns NULL if (typeid(note) != typeid(RealTimeMidiNote))
        if (centerNote != NULL && auxNote != NULL)
        {
            // Get current position of aux key
            map<ps_timestamp, unsigned int>::iterator pos_itr = keyPositionHistory_.end();
            pos_itr--;
            
            //! Get pitch information to scale pitch bends linearly between two pitches
            double auxNotePitch  = noteFreq_;
            double pitchDiff     = auxNotePitch - cntNotePitch;
            
            //! Frequency per division
            double increment        = pitchDiff / (PA_keyDownPositionThreshold - PA_keyIdlePositionThreshold);
            double position         = (double)(*pos_itr).second;
            
            //! Updated (bent) pitch based on aux key position
            double targetBentPitch  = cntNotePitch + increment*(position - PA_keyIdlePositionThreshold);
            
#ifdef DEBUG_SYNTH_PARAMETERS
            cout << "**************************** Pitch Bend Up ***************************"<< endl;
            cout << "Center Note Pitch      = " << cntNotePitch         << "Hz"             << endl;
            cout << "Aux Note Pitch         = " << auxNotePitch         << "Hz"             << endl;
            cout << "Pitch Difference       = " << pitchDiff            << "Hz"             << endl;
            cout << "Increment              = " << increment            << "Hz/pos.unit"    << endl;
            cout << "Aux Key Position       = " << position                                 << endl;
            cout << "Target Bent Pitch      = " << targetBentPitch      << "Hz"             << endl;
            cout << "**********************************************************************"<< endl;
#endif
            centerNote->setAbsolutePitch(cntNotePitch, targetBentPitch, true);
            centerNote->updateSynthParameters();
            
            auxNote->setAbsolutePitch(auxNotePitch, targetBentPitch, true);
            auxNote->updateSynthParameters();
        }
    }
    return rv;
}

bool PNOscanController::PAevent::handleMultiKeyHarmonicSweep()
{
    bool multiKeyGesture = false;
    int centerKey = key_ - 12;
    vector<double> targetHarmonicAmplitudes;
    vector<double> currentHarmonicAmplitudes;
    vector<double> targetAuxHarmonicAmplitudes;
    
    if(centerKey > 0)
    {
        //! Query the state of the key one octave below this note object's MIDI note number
        vector<int> centerKeyStates = PNOcontroller_->getKeyState(centerKey);
        
        //! Look for a down state one octave below this key that occurred before this key's PAnote was created
        if ((centerKeyStates[0] == PA_kKeyStateDown || centerKeyStates[0] == PA_kKeyStateAfterVibrato) && PNOcontroller_->lastDownStateTime_[centerKey] < startTime_)
        {
            multiKeyGesture = true;
            
            //! Find last position and time of the position and velocity histories for the auxiliary note
            map<ps_timestamp, unsigned int>::iterator pos_itr = keyPositionHistory_.end();
            map<ps_timestamp, double>::iterator vel_itr = keyVelocityHistory_.end();
            
            pos_itr--;              //! Decrement the iterators as .end() returns ONE PAST the end
            vel_itr--;
            
            double pos = (*pos_itr).second;
    //        double vel = (*vel_itr).second;
            
            RealTimeMidiNote *centerNote = PNOcontroller_->getNoteForKey(centerKey);
            RealTimeMidiNote *auxNote    = note_;
            
            if(centerNote != NULL && auxNote != NULL)
            {
                //! Get the current values of the harmonic amplitudes specified by the patch
                vector <double> currentHarmonicAmplitudes = centerNote->getCurrentHarmonicAmplitudes();
                
                //! Divide the position difference between Press and Idle by the number of harmonics specified by the patch
                int divisionHeight = (PA_keyDownPositionThreshold - PA_keyIdlePositionThreshold) / 9; //currentHarmonicAmplitudes.size();
                
                //! Activate a harmonic based on which division we're currently in
                int currentActiveHarmonic = ceil((pos - PA_keyIdlePositionThreshold) / divisionHeight);
                
                // We
                targetHarmonicAmplitudes.push_back(0);
                
                // Actuate center and aux note at the same frequency:
                // Center note: Set all harmonics to zero except the active harmonic
                // Aux note:    Set all harmonics to zero except the harmonic below the center note's active harmonic
                for(int i = 0; i < 9; ++i) //i < currentHarmonicAmplitudes.size(); ++i)
                {
                    if (i != currentActiveHarmonic)
                        targetHarmonicAmplitudes.push_back(0);
                    else targetHarmonicAmplitudes.push_back(1);
                    
                    if (i-1 != currentActiveHarmonic)
                        targetAuxHarmonicAmplitudes.push_back(0);
                    else targetAuxHarmonicAmplitudes.push_back(1);
                }
                
                //! Also scale the intensity up to accentuate the effect
                double targetIntensity = (centerNote->getCurrentIntensity()) * 2;
                if (targetIntensity > 1.5)    targetIntensity = 1.5;
                
#ifdef DEBUG_SYNTH_PARAMETERS
                cout                                                                            << endl;
                cout << "********************* Multi-Key Harmonic Sweep **********************" << endl;
                cout << "Division Height         = " << divisionHeight                          << endl;
                cout << "Current Position        = " << pos                                     << endl;
                cout << "Current Active Harmonic = " << currentActiveHarmonic                   << endl;
                cout << "Target Intensity        = " << targetIntensity                         << endl;
                cout << "*********************************************************************" << endl;
                cout                                                                            << endl;
#endif

                centerNote->setRawHarmonicValues(targetHarmonicAmplitudes);
                centerNote->setAbsoluteIntensityBase(targetIntensity);
                centerNote->updateSynthParameters();
            
                auxNote->setRawHarmonicValues(targetAuxHarmonicAmplitudes);
                auxNote->setAbsoluteIntensityBase(targetIntensity);
                auxNote->updateSynthParameters();
            }
        }
    }
    return multiKeyGesture;
}

void PNOscanController::PAevent::sendKeyStateMessages()
{
    //! Make sure the RealTimeMidiNote object exists
    if(note_ != NULL)
    {
        int velSign;
        double pos, vel, acc;
        double intensity, pitch, brightness, pitchVibrato;
        ps_timestamp timeInCurrentState;
        
        int currentState    = PNOcontroller_->keyStates_[key_][0];
        int previousState   = PNOcontroller_->keyStates_[key_][1];
        
        //! Tells us whether a note in pretouch is part of a multi-key gesture
        bool multiKeyPitchBend = false, multiKeyHarmonicSweep = false;
        
        //! Find last position and time of the position, velocity, and acceleration histories
        map<ps_timestamp, unsigned int>::iterator pos_itr = keyPositionHistory_.end();
        map<ps_timestamp, double>::iterator vel_itr = keyVelocityHistory_.end();
        map<ps_timestamp, double>::iterator acc_itr = keyAccelerationHistory_.end();
        
        pos_itr--;              //! Decrement the iterators as .end() returns ONE PAST the end
        vel_itr--;
        acc_itr--;
        
        pos = (*pos_itr).second;
        vel = (*vel_itr).second;
        acc = (*acc_itr).second;
        
        timeInCurrentState = (PNOcontroller_->currentTimeStamp_ - PNOcontroller_->lastStateUpdate_);
        
        switch (currentState)
        {
            case PA_kKeyStateIdle:
                //! Ne fait rien!
                break;
                
            case PA_kKeyStatePretouch:
                //! The multi-key gesture handlers return true if they detect a multi-key gesture, which would make this note the auxiliary note
                multiKeyPitchBend = handleMultiKeyPitchBend();
                multiKeyHarmonicSweep = handleMultiKeyHarmonicSweep();
                
            case PA_kKeyStateTap:
                /*! If the key in pretouch is part of a multi-key gesture, then the above handlers will update the synth parameters rather than those
                    detailed below. */
                if (!multiKeyPitchBend && !multiKeyHarmonicSweep)
                {      
                    intensity = (pos - (double)PA_keyIdlePositionThreshold) / ((double)PA_keyPressPositionThreshold - (double)PA_keyIdlePositionThreshold);
    //                    intensity += fabs(vel) / 10;
                    
                    //! We may be transitioning back and forth between Tap and Pretouch, in which case we want to keep the Tap intensity
                    if(currentState == PA_kKeyStateTap || previousState == PA_kKeyStateTap)	// Give taps an amplitude boost
                        intensity *= 2.0;
                    
                    if(intensity > 1.0) intensity = 1.0;
                    if(intensity < 0.0) intensity = 0.0;
                    
#ifdef DEBUG_SYNTH_PARAMETERS
                    cout << "Setting absolute intensity base to intensity = " << intensity << endl;
#endif
                    note_->setAbsoluteIntensityBase(intensity);
                    note_->updateSynthParameters();
                }
                break;
                
            case PA_kKeyStatePreVibrato:
                
                cout << endl << endl << "================= PA_kKeyStatePreVibrato =================" << endl << endl;
                // Take the absolute value of velocity and use it to set the relative pitch, creating a gliss
                pitch = fabs(avgKeyVelocity_ / 5000.0);
                
                intensity = (pos - (double)PA_keyIdlePositionThreshold) / ((double)PA_keyPressPositionThreshold - (double)PA_keyIdlePositionThreshold);
                
                if(intensity > 1.0) intensity = 1.0;
                if(intensity < 0.0) intensity = 0.0;
                
#ifdef DEBUG_SYNTH_PARAMETERS
                cout << "Setting absolute intensity base to intensity = " << intensity*4 << endl;
#endif
                note_->setAbsoluteIntensityBase(intensity*8);
                
#ifdef DEBUG_SYNTH_PARAMETERS
                cout << "Setting relative pitch base to " << pitch << endl;
#endif
                
                note_->setRelativePitchBase(pitch);
                note_->updateSynthParameters();
                
                break;
                
            case PA_kKeyStatePress:
                //! The multi-key gestures should extend into the 'Press' region
                multiKeyPitchBend = handleMultiKeyPitchBend();
                multiKeyHarmonicSweep = handleMultiKeyHarmonicSweep();
                break;
                
            case PA_kKeyStateDown:
                if(note_->useKeyDownHoldoff())
				{
					if ((vel > PA_keyDownHoldoffVelocity) && (timeInCurrentState < note_->keyDownHoldoffTime()))
					{
						cout << "holdoff time = " << note_->keyDownHoldoffTime() << " scaler = " << note_->keyDownHoldoffScaler() << endl;
						break;
					}
					
					//! If key is held down, make it gradually crescendo
					if(vel > PA_keyDownHoldoffVelocity)
					{
						intensity = timeInCurrentState;
						note_->keyDownHoldoffScaler();
                        
						if(intensity > 1.0) intensity = 1.0;
					}
					else    intensity = 1.0;
				}
				else    intensity = 1.0;
				
                brightness = (pos - 64) / 4;
                if(brightness < 0.0)    brightness = 0.0;
                
#ifdef DEBUG_SYNTH_PARAMETERS
                cout << "Setting absolute brightness to brightness = " << brightness << endl;
#endif
                note_->setAbsoluteBrightness(brightness);
                
#ifdef DEBUG_SYNTH_PARAMETERS
                cout << "Setting absolute intensity base to intensity = " << intensity << endl;
#endif
				note_->setAbsoluteIntensityBase(intensity);
				note_->updateSynthParameters();
                
                break;
                
            case PA_kKeyStateAftertouch:
                
                // Aftertouch has a 7-position range (MIDI 120-127)
                // Use this state to increase brightness
                
                brightness = ( pos - PA_keyAftertouchPositionThreshold ) / ( 127 - PA_keyAftertouchPositionThreshold );
                
                cout << endl << "============== PA_kKeyStateAftertouch ================" << endl;
                cout << "   Setting relative brightness to " << 1 + 2*brightness << endl;
                
                note_->setRelativeBrightness(1 + 4*brightness);
                note_->updateSynthParameters();
                
                break;
                
            case PA_kKeyStateAfterVibrato:
                
                // Best results with fixed relative frequency multipler
                pitchVibrato = 0.0025;
                
                // Bend pitch up while velocity is positive, bend down when velocity is negative
                (vel > 0) ? velSign = 1 : velSign = -1;
                
                pitchVibrato *= velSign;
                
#ifdef DEBUG_SYNTH_PARAMETERS
                cout << "Setting relative pitch base to " << pitchVibrato << endl;
#endif
                note_->setRelativePitchBase(pitchVibrato);
                note_->updateSynthParameters();
                break;
                
            case PA_kKeyStateRelease:
                // ¡No hacer nada!
                break;
                
            case PA_kKeyStateDisabled:
                
                break;
                
            default:
                break;
        }
    }
    else    cerr << "PAevent::sendKeyStateMessages() could not find the RealTimeMidiNote for key " << key_ << endl;
}


// =======================
#pragma mark Sanity Checks
// =======================
void PNOscanController::PAevent::printKeyPositionHistory()
{
    map<ps_timestamp, unsigned int>::iterator itr;
    
    //! Make sure  keyPositionHistory exists
    if (keyPositionHistory_.size() != 0)
    {
        cout << "Key " << key_ << " position history:\n";
        
        for (itr = keyPositionHistory_.begin(); itr != keyPositionHistory_.end(); ++itr)
        {
            cout << (*itr).second << '\t' << "(t = " << (*itr).first << ")\n";
        }
    }
    else cout << "Key " << key_ << " has no position history.\n";
}

void PNOscanController::PAevent::printCurrentMotionFeatures()
{
    //! Initialize iterators to end (most recent updates) of each map containing key gesture features
    map<ps_timestamp, unsigned int>::const_iterator pos_itr = keyPositionHistory_.end();
    map<ps_timestamp, double>::const_iterator vel_itr = keyVelocityHistory_.end();
    map<ps_timestamp, double>::const_iterator acc_itr = keyAccelerationHistory_.end();
    
    //! Can't forget that map iterator member end() returns ONE PAST the last element
    pos_itr--;
    vel_itr--;
    acc_itr--;
    
    vector<int>keyStates_ = PNOcontroller_->getKeyState(key_);
    int currentState  = keyStates_[0];
    int previousState = keyStates_[1];
    
    //! For displaying state information on the terminal
    string currentStateString = PNOcontroller_->kKeyStateToString(currentState);
    string previousStateString = PNOcontroller_->kKeyStateToString(previousState);
    
    cout    << "Key " << key_ << endl
    << '\t' << setw(5) << "Pos" << '\t' << setw(6) << "Avg Pos"
    << '\t' << setw(9) << "Vel" << '\t' << setw(9) << "Avg Vel"
    << '\t' << setw(10) << "Acc" << '\t' << setw(10) << "Avg Acc" << '\t' << setw(10) << "Pk Acc"
    << '\t' << setw(10) << "Previous State" << '\t' << setw(10) << "Current State" << endl;
    
    cout    << setprecision(4);
    cout    << '\t' << setw(5) << (*pos_itr).second << '\t' << setw(6) << avgKeyPosition_
    << '\t' << setw(9) << (*vel_itr).second << '\t' << setw(9) << avgKeyVelocity_
    << '\t' << setw(10) << (*acc_itr).second << '\t' << setw(10) << avgKeyAcceleration_ << '\t' << setw(10) << peakKeyAcceleration_
    << '\t' << setw(10) << previousStateString << '\t' << setw(10) << currentStateString
    << "\t (t = " << setw(8) << setprecision(5) << (*pos_itr).first << ")\n";
}

void PNOscanController::PAevent::printKeyMotionHistories()
{
    //! Initialize iterators to begining (earliest timestamp key) of each map
    map<ps_timestamp, unsigned int>::iterator pos_itr = keyPositionHistory_.begin();
    map<ps_timestamp, double>::iterator vel_itr = keyVelocityHistory_.begin();
    map<ps_timestamp, double>::iterator acc_itr = keyAccelerationHistory_.begin();
    map<ps_timestamp, double>::iterator aph_itr = avgKeyPositionHistory_.begin();
    map<ps_timestamp, double>::iterator avh_itr = avgKeyVelocityHistory_.begin();
    map<ps_timestamp, double>::iterator aah_itr = avgKeyAccelerationHistory_.begin();
    map<ps_timestamp, double>::iterator pah_itr = peakKeyAccelerationHistory_.begin();
    
    /*! Make sure the histories exist.  We only check the position history because the other histories are only updated
     when the position history is updated. */
    if (keyPositionHistory_.size() != 0)
    {
        cout << "Key " << key_ << endl
        << '\t' << setw(8) << "Pos" << '\t' << setw(8) << "Avg Pos"
        << '\t' << setw(9) << "Vel" << '\t' << setw(9) << "Avg Vel"
        << '\t' << setw(9) << "Acc" << '\t' << setw(9) << "Avg Acc"
        << '\t' << setw(9) << "Pk Acc" << '\t' << setw(8) << "Key State" << '\t' << "Time Stamp" << endl;
        
        /*! Again, because acceleration and velocity histories are updated whenever the position history is updated, we
         assume that we can read off the timestamp from just the position history. */
        while (pos_itr != keyPositionHistory_.end())
        {
            cout << setprecision(4);
            
            cout << '\t' << setw(8) << (*pos_itr).second << '\t' << setw(8) << (*aph_itr).second
            << '\t' << setw(9) << (*vel_itr).second << '\t' << setw(9) << (*avh_itr).second
            << '\t' << setw(10) << (*acc_itr).second << '\t' << setw(10) << (*aah_itr).second << '\t' << setw(10) << (*pah_itr).second
            << '\t' << setw(8) << keyStateChangeHistory_[(*pos_itr).first] << "\t(t = " << setprecision(5) << (*pos_itr).first << ")\n";
            
            pos_itr++;
            vel_itr++;
            acc_itr++;
            aph_itr++;
            avh_itr++;
            aah_itr++;
            pah_itr++;
        }
    }
    else cout << "Key " << key_ << " has no position history.\n";
}

void PNOscanController::PAevent::writeKeyMotionHistoriesToCSV()
{
    //! Construct the filename for writing motion histories to a .csv file (there's got to be a better way to do this)
    stringstream midiNoteString;                                            //! Initialize stringstream object
    midiNoteString << key_;                                                 //! Convert int to string
    string CSVfileString = "Key";                                           //! First part of filename
    CSVfileString.append(midiNoteString.str());                             //! Append MIDI note number
    CSVfileString.append(".csv");                                           //! Append file type
    string::iterator itr = CSVfileString.begin();                           //! String iterator for string to const char* conversion
    const char *CSVfilename[16];
    
    for(int i = 0; i <= CSVfileString.length(); ++i)
    {
        CSVfilename[i] = &*itr;                                             
        itr++;
    }
    
    //! Initialize the file output object
    ofstream motionHistories;
    
    //! Create the empty .csv file
    motionHistories.open(*CSVfilename);
    
    //! Initialize iterators to begining (earliest timestamp key) of each map
    map<ps_timestamp, unsigned int>::iterator pos_itr = keyPositionHistory_.begin();
    map<ps_timestamp, double>::iterator vel_itr = keyVelocityHistory_.begin();
    map<ps_timestamp, double>::iterator acc_itr = keyAccelerationHistory_.begin();
    map<ps_timestamp, double>::iterator aph_itr = avgKeyPositionHistory_.begin();
    map<ps_timestamp, double>::iterator avh_itr = avgKeyVelocityHistory_.begin();
    map<ps_timestamp, double>::iterator aah_itr = avgKeyAccelerationHistory_.begin();
    map<ps_timestamp, double>::iterator pah_itr = peakKeyAccelerationHistory_.begin();
    
    /*! Make sure the histories exist.  We only check the position history because the other histories are only updated
     when the position history is updated. */
    if (keyPositionHistory_.size() != 0 && motionHistories.is_open())
    {
        //! Title of each column
        motionHistories << "Position,Avg. Position,Velocity,Avg. Velocity,Acceleration,Avg. Acceleration,Peak Acceleration,Timestamp\n";
        
        /*! Again, because acceleration and velocity histories are updated whenever the position history is updated, we
         assume that we can read off the timestamp from just the position history. */
        while (pos_itr != keyPositionHistory_.end())
        {
            motionHistories << (*pos_itr).second << ',' << (*aph_itr).second << ',' << (*vel_itr).second << ',' << (*avh_itr).second << ','
            << (*acc_itr).second << ',' << (*aah_itr).second << ',' << (*pah_itr).second << ',' << (*pos_itr).first << endl;
            pos_itr++;
            vel_itr++;
            acc_itr++;
            aph_itr++;
            avh_itr++;
            aah_itr++;
            pah_itr++;
        }
        motionHistories.close();
    }
    else cout << "Unable to open file" << CSVfilename << endl;
}