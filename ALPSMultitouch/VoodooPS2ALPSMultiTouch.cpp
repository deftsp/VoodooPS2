/*
 * Copyright (c) 2002 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.2 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <IOKit/assert.h>
#include <IOKit/IOLib.h>
#include <IOKit/hidsystem/IOHIDParameter.h>
#include "VoodooPS2ALPSMultiTouch.h"

#define DEBUG 0

#if DEBUG 
#define DEBUG_LOG(fmt, args...) IOLog("[%s] "fmt"\n", getName(), ## args)
#else
#define DEBUG_LOG(fmt, args...) 
#endif

#define ARRAY_SIZE(x)	(sizeof(x)/sizeof(x[0]))
enum {
    //
    //
    kTapEnabled  = 0x01
};

int ScrollDelayCount = 0;
int tfsfactor = 2; //TwoFingerScroll Factor

// =============================================================================
// ApplePS2ALPSMultiTouch Class Implementation
//

#define super IOHIPointing

OSDefineMetaClassAndStructors(ApplePS2ALPSMultiTouch, IOHIPointing);
UInt32 ApplePS2ALPSMultiTouch::deviceType() { return NX_EVS_DEVICE_TYPE_MOUSE; };
UInt32 ApplePS2ALPSMultiTouch::interfaceID(){ return NX_EVS_DEVICE_INTERFACE_BUS_ACE; };
IOItemCount ApplePS2ALPSMultiTouch::buttonCount() { return 2; };
IOFixed     ApplePS2ALPSMultiTouch::resolution()  { return _resolution; };

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// Added for Loading Tap Settings at boot.
bool TapSettingsLoaded = false;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

bool ApplePS2ALPSMultiTouch::init( OSDictionary * properties )
{
    //
    // Initialize this object's minimal state. This is invoked right after this
    // object is instantiated.
    //
    
    if (!super::init(properties))  return false;
    _device                    = 0;
    _interruptHandlerInstalled = false;
    _packetByteCount           = 0;
    _resolution                = (100) << 16; // (100 dpi, 4 counts/mm) On init should be on default
    _touchPadModeByte          = kTapEnabled;
    _scrolling                 = SCROLL_NONE;
    _zscrollpos                = 0;
    z_finger=30;
    divisor=1; // Standard was 23, changed for high res fix
    ledge=1700;
    redge=5200;
    tedge=4200;
    bedge=1700;
    vscrolldivisor=30;
    hscrolldivisor=30;
    cscrolldivisor=0;
    ctrigger=0;
    centerx=3000;
    centery=3000;
    lastx=0;
    lasty=0;
    xrest=0; 
    yrest=0; 
    scrollrest=0;
    inited=0;
    maxtaptime=200000000;
    clicking=true;
    maxdragtime=300000000;
    dragging=false;
    draglock=false;
    hscroll=false;
    scroll=true;
    hsticky=0;
    vsticky=0;
    wsticky=1;
    tapstable=1;
    wlimit=9;
    wvdivisor=30;
    whdivisor=30;
    xmoved=ymoved=xscrolled=yscrolled=0;
    touchmode=MODE_NOTOUCH;
    wasdouble=false;

    return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

ApplePS2ALPSMultiTouch *
ApplePS2ALPSMultiTouch::probe( IOService * provider, SInt32 * score )
{
    //
    // The driver has been instructed to verify the presence of the actual
    // hardware we represent. We are guaranteed by the controller that the
    // mouse clock is enabled and the mouse itself is disabled (thus it
    // won't send any asynchronous mouse data that may mess up the
    // responses expected by the commands we send it).
    //

//  const unsigned char rates[] = {0, 10, 20, 40, 60, 80, 100, 200}; // From Linux
    bool success = false;
    UInt8 Byte1, Byte2, Byte3;

    if (!super::probe(provider, score)) return 0;

    _device = (ApplePS2MouseDevice *) provider;

    PS2Request * request = _device->allocateRequest();

    if ( !request ) return 0;

    // "E6 report"
    request->commands[0].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[0].inOrOut = kDP_SetMouseResolution;
    request->commands[1].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[1].inOrOut = 0;

    // 3X set mouse scaling 1 to 1
    request->commands[2].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[2].inOrOut = kDP_SetMouseScaling1To1;
    request->commands[3].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[3].inOrOut = kDP_SetMouseScaling1To1;
    request->commands[4].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[4].inOrOut = kDP_SetMouseScaling1To1;
    request->commands[5].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[5].inOrOut = kDP_GetMouseInformation;
    request->commands[6].command = kPS2C_ReadDataPort;
    request->commands[6].inOrOut = 0;
    request->commands[7].command = kPS2C_ReadDataPort;
    request->commands[7].inOrOut = 0;
    request->commands[8].command = kPS2C_ReadDataPort;
    request->commands[8].inOrOut = 0;
    request->commandsCount = 9;
    _device->submitRequestAndBlock(request);

    // result is "E6 Report"
    Byte1 = request->commands[6].inOrOut;
    Byte2 = request->commands[7].inOrOut;
    Byte3 = request->commands[8].inOrOut;
    _device->freeRequest(request);
    DEBUG_LOG("E6 Report: [ 0x%02x, 0x%02x, 0x%02x ]\n", Byte1, Byte2, Byte3);
 
    request = _device->allocateRequest();
    if (!request) return 0;

    // Now fetch "E7 Report"
    request->commands[0].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[0].inOrOut = kDP_SetMouseResolution;
    request->commands[1].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[1].inOrOut = 0;

    // 3X set mouse scaling 2 to 1
    request->commands[2].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[2].inOrOut = kDP_SetMouseScaling2To1;
    request->commands[3].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[3].inOrOut = kDP_SetMouseScaling2To1;
    request->commands[4].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[4].inOrOut = kDP_SetMouseScaling2To1;
    request->commands[5].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[5].inOrOut = kDP_GetMouseInformation;
    request->commands[6].command = kPS2C_ReadDataPort;
    request->commands[6].inOrOut = 0;
    request->commands[7].command = kPS2C_ReadDataPort;
    request->commands[7].inOrOut = 0;
    request->commands[8].command = kPS2C_ReadDataPort;
    request->commands[8].inOrOut = 0;
    request->commandsCount = 9;
    _device->submitRequestAndBlock(request);
    Byte1 = request->commands[6].inOrOut;
    Byte2 = request->commands[7].inOrOut;
    Byte3 = request->commands[8].inOrOut;
    _device->freeRequest(request);

    DEBUG_LOG("E7 Report: [ 0x%02x, 0x%02x, 0x%02x ]\n", Byte1, Byte2, Byte3);

    if (Byte1 == 0x73 || Byte2 == 0x02 || Byte3 == 0x64)  // ALPS MultiTouch check
    {
        success = true;
    }

    DEBUG_LOG("ALPS MoultiTouch? %s", (success ? "Yes" : "No"));

    int v1, v2;
    v1 = Byte3 & 0x0f; // Apple use 0x0f
    v2 = Byte1;
    _touchPadVersion = (v1<<8) | v2;

    // tried linux versioning, aborted since i dunno why is diff then Apple
    //int i;
    //for (i = 0; i < ARRAY_SIZE(rates) && Byte3 != rates[i]; i++)
    //  /* empty */;
    //_touchPadVersion = (Byte1 << 8) | (Byte2 << 4) | i;

    if (success) 
    {
        OSDictionary *Configuration;        
        setProperty ("Revision", 24, 32);
        Configuration = OSDynamicCast(OSDictionary, getProperty("Configuration"));
        if (Configuration){
            OSString *tmpString = 0;
            OSNumber *tmpNumber = 0;
            OSData   *tmpData = 0;
            OSBoolean *tmpBoolean = false;
            OSData   *tmpObj = 0;
            UInt32 tmpUI32;
            OSIterator *iter = 0;
            const OSSymbol *dictKey = 0;
            iter = OSCollectionIterator::withCollection(Configuration);
            if (iter) {
                while ((dictKey = (const OSSymbol *)iter->getNextObject())) {
                    tmpObj = 0;
                    
                    tmpString = OSDynamicCast(OSString, Configuration->getObject(dictKey));
                    if (tmpString) {
                        tmpObj = OSData::withBytes(tmpString->getCStringNoCopy(), tmpString->getLength()+1);
                    }
                    
                    tmpNumber = OSDynamicCast(OSNumber, Configuration->getObject(dictKey));
                    if (tmpNumber) {
                        tmpUI32 = tmpNumber->unsigned32BitValue();
                        tmpObj = OSData::withBytes(&tmpUI32, sizeof(UInt32));
                    }
                    
                    tmpBoolean = OSDynamicCast(OSBoolean, Configuration->getObject(dictKey));
                    if (tmpBoolean) {
                        bool tmpB = (bool)tmpBoolean->getValue();
                        tmpObj = OSData::withBytes(&tmpB, sizeof(bool));
                    }
                    
                    tmpData = OSDynamicCast(OSData, Configuration->getObject(dictKey));
                    if (tmpData) {
                        tmpObj = tmpData;
                    }
                    
                    if (tmpObj) {
                        provider->setProperty(dictKey, tmpObj);
                    }
                }
            }                   
        }       
    }

    return (success) ? this : 0;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

bool ApplePS2ALPSMultiTouch::start( IOService * provider )
{
    UInt64 enabledProperty;

    //
    // If this is not here, then when it starts it takes mouse acceleration until changed
    // from trackpad preference pane
    //

    setProperty(kIOHIDPointerAccelerationTypeKey, kIOHIDTrackpadAccelerationType);

    //
    // The driver has been instructed to start. This is called after a
    // successful probe and match.
    //

    if (!super::start(provider)) return false;

    //
    // Maintain a pointer to and retain the provider object.
    //

    _device = (ApplePS2MouseDevice *) provider;
    _device->retain();

    //
    // Announce hardware properties.
    //

    IOLog("ApplePS2Trackpad: ALPS MultiTouch v%d.%d\n",
          (UInt8)(_touchPadVersion >> 8), (UInt8)(_touchPadVersion));

    //
    // IntelliMouse Mode until we figure the new EC Absolute mode format...
    //

    setIntelliMouseMode();

    //
    // Advertise some supported features (tapping, edge scrolling).
    //

    enabledProperty = 1;

    // Enable tapping

    setTapEnable( true );

    // Enable Absolute Mode

    if(_packetByteCount == 6)
    {
        setAbsoluteMode();
    }

    //
    // Must add this property to let our superclass know that it should handle
    // trackpad acceleration settings from user space.  Without this, tracking
    // speed adjustments from the mouse prefs panel have no effect.
    //

    setProperty(kIOHIDPointerAccelerationTypeKey, kIOHIDTrackpadAccelerationType);

    //
    // Install our driver's interrupt handler, for asynchronous data delivery.
    //

    _device->installInterruptAction(this,
        OSMemberFunctionCast(PS2InterruptAction,this,&ApplePS2ALPSMultiTouch::interruptOccurred));
    _interruptHandlerInstalled = true;

    //
    // Enable the mouse clock (should already be so) and the mouse IRQ line.
    //

    setCommandByte( kCB_EnableMouseIRQ, kCB_DisableMouseClock );

    //
    // Finally, we enable the trackpad itself, so that it may start reporting
    // asynchronous events.
    //

    setTouchPadEnable(true);

    //
    // Install our power control handler.
    //

    _device->installPowerControlAction( this, OSMemberFunctionCast(PS2PowerControlAction,this,
             &ApplePS2ALPSMultiTouch::setDevicePowerState) );
    _powerControlHandlerInstalled = true;

    return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ALPSMultiTouch::stop( IOService * provider )
{
    //
    // The driver has been instructed to stop.  Note that we must break all
    // connections to other service objects now (ie. no registered actions,
    // no pointers and retains to objects, etc), if any.
    //

    assert(_device == provider);

    //
    // Disable the mouse itself, so that it may stop reporting mouse events.
    //

    setTouchPadEnable(false);

    //
    // Disable the mouse clock and the mouse IRQ line.
    //

    setCommandByte( kCB_DisableMouseClock, kCB_EnableMouseIRQ );

    //
    // Uninstall the interrupt handler.
    //

    if ( _interruptHandlerInstalled )  _device->uninstallInterruptAction();
    _interruptHandlerInstalled = false;

    //
    // Uninstall the power control handler.
    //

    if ( _powerControlHandlerInstalled ) _device->uninstallPowerControlAction();
    _powerControlHandlerInstalled = false;

	super::stop(provider);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ALPSMultiTouch::free()
{
    //
    // Release the pointer to the provider object.
    //

    if (_device)
    {
        _device->release();
        _device = 0;
    }

    super::free();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ALPSMultiTouch::interruptOccurred( UInt8 data )
{
    //
    // This will be invoked automatically from our device when asynchronous
    // events need to be delivered. Process the trackpad data. Do NOT issue
    // any BLOCKING commands to our device in this context.
    //
    // Ignore all bytes until we see the start of a packet, otherwise the
    // packets may get out of sequence and things will get very confusing.
    //
		
    if (_packetByteCount == 0 && ((data == kSC_Acknowledge) || !(data & 0x08)))
    {
        return;
    }

    _packetBuffer[_packetByteCount++] = data;
    /*
	if((_packetBuffer[0] & 0xc8) == 0x08) {
		if(_packetByteCount == 3) {
        dispatchRelativePointerEventWithPacket(_packetBuffer, 3);
        _packetByteCount = 0;
			return;
		}
		return;
    }
	if((_packetBuffer[0] & 0xf8) != 0xf8) {
		IOLog("Bad data: %d bytes\n",(int)_packetByteCount);
		_packetByteCount = 0;
		return; //bad data.
	}
	if(_packetByteCount >= 2 && _packetByteCount <=6 && _packetBuffer[_packetByteCount-1] == 0x80)
	{
		IOLog("Bad data2: %d bytes\n",(int)_packetByteCount);
		_packetByteCount = 0;
		return; //bad data
	}
	if(_packetByteCount == 3) // Normal PS/2 mouse mode
	{
		dispatchRelativePointerEventWithPacket(_packetBuffer,3);
		_packetByteCount = 0;
		return;
	}
	*/
	if(_packetByteCount == 4) // IntelliMouse Mode
	{
		dispatchRelativePointerEventWithPacket(_packetBuffer,4);
		_packetByteCount = 0;
		return;
	}
	
	if(_packetByteCount == 6) // Absolute mode
	{
		dispatchAbsolutePointerEventWithPacket(_packetBuffer,6);
		_packetByteCount = 0;
		return;
	}
	return;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ALPSMultiTouch::dispatchAbsolutePointerEventWithPacket(UInt8* packet, UInt32 packetSize)
{
    // PS/2 packet format:
    //
    // 6-byte movement data packet (ALPS Absolute Mode):
    //        Bit7  Bit6  Bit5  Bit4  Bit3  Bit2  Bit1  Bit0  
    //        ------------------------------------------------
    // Byte1: 1     ?     ?     ?     1     ?     ?     ?
    // Byte2: 0     x6    x5    x4    x3    x2    x1    x0
    // Byte3: 0     x10   x9    x8    x7    ?     fin   ges
    // Byte4: 0     y9    y8    y7    1     mb    rb    lb
    // Byte5: 0     y6    y5    y4   y3     y2    y1    y0
    // Byte6: 0     z6    z5    z4   z3     z2    z1    z0
    //
    // 9-byte movement data packet (ALPS Interleaved Mode): // This should be handled separate
    //        Bit7  Bit6  Bit5  Bit4  Bit3  Bit2  Bit1  Bit0  
    //        ------------------------------------------------
    // Byte1: 1     1     1     0     0     1     1     1
    // Byte2: 0     x6    x5    x4    x3    x2    x1    x0
    // Byte3: 0     x10   x9    x8    x7    0     fin   ges
    // Byte4: 0     0     YS    XS    1     1     1     1
    // Byte5: X7    X6    X5    X4    X3    X2    X1    X0
    // Byte6: Y7    Y6    Y5    Y4    Y3    Y2    Y1    Y0
    // Byte7: 0     y9    y8    y7    1     mb    rb    lb
    // Byte8: 0     y6    y5    y4    y3    y2    y1    y0
    // Byte9: 0     z6    z5    z4    z3    z2    z1    z0
    //
    // Legend:
    // L = left R = right M = middle B = button
    // *S's = sign bit
    // CAPITALS = stick, miniscules = touchpad
    // 0 = Always 0
    // 1 = Always 1
    // ?'s can have different meanings on different models,
    // such as wheel rotation, extra buttons, stick buttons
    // on a dualpoint, etc.
    //
    
    UInt32 buttons = 0;
    int left = 0, right = 0, middle = 0;
	int tap = 0, tapclick = 0;
    int xdiff, ydiff, scroll, s_xdiff, s_ydiff, s_ref_x, s_ref_y, tfsf2;

    //uint64_t now;
	AbsoluteTime now;
    bool wasNotScrolling, willScroll = false, twoFingerScroll;

	twoFingerScroll = false;
	s_ref_x =950;
	s_ref_y =950;

    int x = (packet[1] & 0x7f) | ((packet[2] & 0x78) << (7-3));
    int y = (packet[4] & 0x7f) | ((packet[3] & 0x70) << (7-4));
    int z = packet[5]; // touch pression
	
	xdiff = x - _xpos;
	ydiff = y - _ypos;

	DEBUG_LOG("Packets: 0x%02x - 0x%02x - 0x%02x - 0x%02x - 0x%02x - 0x%02x\n",
			  (unsigned int)packet[0], (unsigned int)packet[1], (unsigned int)packet[2], 
			  (unsigned int)packet[3], (unsigned int)packet[4], (unsigned int)packet[5]);
	//IOSleep(20);

#if APPLESDK
	clock_get_uptime(&now);
#else 
	clock_get_uptime((uint64_t*)&now);
#endif
    
    left  |= (packet[3]) & 1;
    right |= (packet[3] >> 1) & 1;
	middle |= left & right;
	tap = (packet[2] >> 1) & 1;
	tapclick = packet[2] & 1;
    buttons |= left ? 0x01 : 0;
    buttons |= right ? 0x02 : 0;
    buttons |= middle ? 0x04 : 0;

    DEBUG_LOG("Absolute packet: x: %d, y: %d, xpos: %d, ypos: %d, buttons: %x, "
              "z: %d, zpos: %d\n", x, y, (int)_xpos, (int)_ypos, (int)buttons, 
              (int)z, (int)_zpos);
//	scroll = false;

    wasNotScrolling = _scrolling == SCROLL_NONE;
    scroll = insideScrollArea(x, y);

    if ((z >= 100) && (_edgehscroll || _edgevscroll)) //Z value increases as more trackpad area is touched 
	{												  //I've determined this value using my fingers.	
		twoFingerScroll = true;
		s_ref_x = x;
		s_ref_y = y;
	}
//Slice
/*	z=
 30..55 - left edge
 60..80 - bottom edge
 70..90 - centre
 >100 - two finger
	
*/	
	if ((z>20) && (z<69)) {
		//willScroll = true;
		willScroll = ((scroll & SCROLL_VERT)  && _edgevscroll) || 
					 ((scroll & SCROLL_HORIZ) && _edgehscroll) ;

	} 
	DEBUG_LOG("Absolute packet: tap: %d  tapclick: %d scroll: %d willScroll: %d\n", tap, tapclick, scroll, willScroll);

#if VOODOO 
	
    /*willScroll = ((scroll & SCROLL_VERT) && _edgevscroll) || 
                    ((scroll & SCROLL_HORIZ) && _edgehscroll);*/
	willScroll = _zpos > 0;

	willScroll &= ((scroll & SCROLL_VERT) && _edgevscroll) || 
		((scroll & SCROLL_HORIZ) && _edgehscroll) ;

	if (willScroll)
		ScrollDelayCount++;   //Inc the delay count this stops scrolling from accidental scroll region touches

    // Make sure we are still relative
    if (z == 0 || (_zpos >= 1 && z != 0 && !willScroll && !twoFingerScroll))
    {
        _xpos = x;
        _ypos = y;
    }
    
	// I added this for tapping but it didn't work	
	//	if ((wasNotScrolling) && (willScroll) && (!twoFingerScroll) && ((x > 950) && (y < 100)))  ScrollDelayCount = 21; //tap

    // Are we scrolling?
    if (((willScroll) && (ScrollDelayCount > 20)) || (!wasNotScrolling)) // Only scroll when delay count reached
    {
        if (_zscrollpos <= 0 || wasNotScrolling)
        {
            _xscrollpos = x;
            _yscrollpos = y;
        }
        
        xdiff = x - _xscrollpos;
        ydiff = y - _yscrollpos;

		s_ydiff = (scroll == SCROLL_VERT) ? -((int)((double)ydiff * _edgeaccellvalue)) : 0;
        s_xdiff = (scroll == SCROLL_HORIZ) ? -((int)((double)xdiff * _edgeaccellvalue)) : 0;

		_xscrollpos = x;
		_yscrollpos = y;

		ydiff = -((int)((double)ydiff * _edgeaccellvalue));
        xdiff = -((int)((double)xdiff * _edgeaccellvalue));
		DEBUG_LOG(" ABmod : Sensed EdgeScrolling z:%d,_zpos:%d: s_xdiff:%d, s_ydiff:%d, x:%d, y:%d, xdiff:%d, ydiff:%d\n",
				  (int)z,(int)_zpos, s_xdiff, s_ydiff,(int)x,(int)y, (int) xdiff, (int) ydiff);
		
        dispatchScrollWheelEvent( ((scroll & SCROLL_VERT) ? ydiff : 0), ((scroll & SCROLL_HORIZ) ? xdiff : 0), 0, time);
        _zscrollpos = z;
		ScrollDelayCount = 21; //set to 21 so we don't increment out of integer range.

        return;
    }

	if (twoFingerScroll) {   //Now we have the two finger scroll code	
		ScrollDelayCount++;
		
		xdiff = x - _xpos;
		ydiff = y - _ypos;
		_xpos = x;
		_ypos = y;

		s_xdiff = 0; //reset
		s_ydiff = 0; //reset
		
		if (xdiff > 0)
			s_xdiff = -1;	//Not very good code but it works, sorry devs ;)

		if (xdiff < 0)
			s_xdiff = 1;		//
										//
		if (ydiff > 0)
			s_ydiff = -1;	//

		if (ydiff < 0)
			s_ydiff = 1;		//Crude, yes
		
		if (!_edgevscroll)
			s_ydiff = 0;  //is Vertical Scrolling on in Trackpad.prefpane?
		
		if (!_edgehscroll)
			s_xdiff = 0; //is Horizontal Scrolling on in Trackpad.prefpane?
			
		//IOLog for debug use only

		DEBUG_LOG("ABmod : Sensed 2 Fingerscrolling z:%d,_zpos:%d: s_xdiff:%d, s_ydiff:%d, x:%d, y:%d, xdiff:%d, ydiff:%d\n",
				  (int)z,(int)_zpos, s_xdiff, s_ydiff,(int)x,(int)y, (int) xdiff, (int) ydiff);

		if (ScrollDelayCount>3)  //We have a delay in this also, just incase of accidental two finger presses
		{
			tfsf2 = (int)(tfsfactor + (int)((int)_edgeaccell/(256*16)));  //Value from Trackpad.prefpanes
			dispatchScrollWheelEvent(s_ydiff*tfsf2, s_xdiff*tfsf2, 0, time);  //Multiply with a factor
			ScrollDelayCount = 0;												//Reset Delay
		}
		_scrolling = SCROLL_VERT;	//Had to assign a scroll value.
		_zscrollpos = z;			//report we are scrolling
		
		return;						//Return
	}

    _zpos = z == 0 ? _zpos + 1 : 0;
    _scrolling = SCROLL_NONE;
    
    xdiff = x - _xpos;
    ydiff = y - _ypos;
    
    _xpos = x;
    _ypos = y;
    
    DEBUG_LOG("Sending event: %d,%d,%d\n",xdiff,ydiff,(int)buttons);
    //dispatchRelativePointerEvent(xdiff, ydiff, buttons, time);

	if ((willScroll) || (twoFingerScroll)) {
		xdiff = 0;
		ydiff = 0;
	}
	
	if (ScrollDelayCount < 5)  //Just works??	
		dispatchRelativePointerEvent(xdiff, ydiff, buttons, now);

	if (!willScroll)
		ScrollDelayCount = 0;
#else
	if (willScroll)
		ScrollDelayCount++;   //Inc the delay count this stops scrolling from accidental scroll region touches
	_movedelay++;
	if (tap && (!willScroll) && !tapclick) {
		if (_movedelay > 3) {
			xdiff = x - _xpos;
			ydiff = y - _ypos;
			_movedelay = 4;
		} else {
			xdiff = 0;
			ydiff = 0;
		}
		
		_xpos = x;
		_ypos = y;	
		touchmode = MODE_MOVE;
		dispatchRelativePointerEvent(xdiff, ydiff, buttons, now);
		_time = now;
	}
		
	if (tap && (willScroll) && (ScrollDelayCount>10)){
		ScrollDelayCount=11;
		
		xdiff = x - _xpos;
		ydiff = y - _ypos;
		_xpos = x;
		_ypos = y;
		
		s_xdiff = 0; //reset
		s_ydiff = 0; //reset
		if (xdiff && (scroll & SCROLL_HORIZ) && _edgehscroll) { //is Horizontal Scrolling on in Trackpad.prefpane?
			s_xdiff = (xdiff > 0)?(-1):1;
		}
		if (ydiff && (scroll & SCROLL_VERT) && _edgevscroll){ //is Vertical Scrolling on in Trackpad.prefpane?
			s_ydiff = (ydiff > 0)?(-1):1;
		}
		
		tfsf2 = (int)(tfsfactor + (int)((int)_edgeaccell/(256*16)));  //Value from Trackpad.prefpanes
		dispatchScrollWheelEvent(s_ydiff*tfsf2, s_xdiff*tfsf2, 0, now);  //Multiply with a factor	
		touchmode = MODE_VSCROLL;
	}
	
	if (!tap) {
		/*if ((touchmode == MODE_MOVE) && ((*(uint64_t*)&now -*(uint64_t*)&time)<maxtaptime)) {
			dispatchRelativePointerEvent(0,0,1,now);
			IODelay(1000);
			clock_get_uptime((uint64_t*)&now);
			dispatchRelativePointerEvent(0,0,0,now);
		}*/
		_movedelay = 0;
		touchmode = MODE_NOTOUCH;
	}
	
	if (tapclick) {
		touchmode = MODE_MTOUCH;
		dispatchRelativePointerEvent(0,0,1,now);
#if DEBUG		
		uint64_t diff = (*(uint64_t*)&now -*(uint64_t*)&_time);
#endif		
		DEBUG_LOG(" tapclick with diff=%ld while max=%ld\n", (long int)diff, (long int)maxtaptime);
		IODelay(1000);
#if APPLESDK
		clock_get_uptime(&now);
#else 
		clock_get_uptime((uint64_t*)&now);
#endif
		dispatchRelativePointerEvent(0,0,0,now);					
		_time = now;
	}
	if (!tapclick && (touchmode == MODE_MTOUCH)) {
		uint64_t diff = (*(uint64_t*)&now -*(uint64_t*)&_time);
		DEBUG_LOG(" tapclick with diff=%ld while max=%ld\n", (long int)diff, (long int)maxtaptime);
		if (diff < maxtaptime) {
			dispatchRelativePointerEvent(0,0,1,now);
			IODelay(1000);
#if APPLESDK
			clock_get_uptime(&now);
#else 
			clock_get_uptime((uint64_t*)&now);
#endif
			dispatchRelativePointerEvent(0,0,0,now);			
		}
		touchmode = MODE_NOTOUCH;
	}
		
	if (!willScroll)
		ScrollDelayCount = 0;
	
#endif	
	return;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

int ApplePS2ALPSMultiTouch::insideScrollArea(int x, int y)
{
    int scroll = SCROLL_NONE;
    if (x > 900) scroll |= SCROLL_VERT;
    if (y > 650) scroll |= SCROLL_HORIZ;
    
    if (x > 900 && y > 650)
    {
        if (_scrolling == SCROLL_VERT)
            scroll = SCROLL_VERT;
        else
            scroll = SCROLL_HORIZ;
    }
    
    _scrolling = scroll;
    return scroll;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ALPSMultiTouch::dispatchRelativePointerEventWithPacket( UInt8 * packet, UInt32  packetSize )
{
    // PS/2 packet format:
    //
    // 3-byte movement data packet (Standard PS/2 Mouse): // In case EC or Intellimouse emulation mode fail, this works...
    //
    //        Bit7  Bit6  Bit5  Bit4  Bit3  Bit2  Bit1  Bit0  
    //        ------------------------------------------------
    // Byte1: YO    XO    YS    XS    1     MB    RB    LB    
    // Byte2: X7    X6    X5    X4    X3    X3    X1    X0    (X delta)
    // Byte3: Y7    Y6    Y5    Y4    Y3    Y2    Y1    Y0    (Y delta)
    //
    // 4-byte movement data packet (3-button Intellimouse): // In case we want to emulate it
    //
    // Byte4: ZS    ZS    ZS    ZS    Z3    Z2    Z1    Z0    (Z delta)
    //
    // 4-byte movement data packet (5-button Intellimouse): // Just for reference...
    //
    // Byte4: 0     0     5B    4B    Z3    Z2    Z1    Z0    (Z delta)
    //
    // Legend:
    // L = left R = right M = middle B = button
    // *O's = overflow Note that the device never signals overflow condition.
    // *S's = sign bit
    // 0 = Always 0
    // 1 = Always 1

    UInt32       buttons = 0;
    SInt32       dx, dy;
    SInt16       dz = 0;
    AbsoluteTime now;

    if ( (packet[0] & 0x1) ) buttons |= 0x1;  // left button   (bit 0 in packet)
    if ( (packet[0] & 0x2) ) buttons |= 0x2;  // right button  (bit 1 in packet)
    if ( (packet[0] & 0x4) ) buttons |= 0x4;  // middle button (bit 2 in packet)
    dx = ((packet[0] & 0x10) ? 0xffffff00 : 0 ) | packet[1];
    dy = -(((packet[0] & 0x20) ? 0xffffff00 : 0 ) | packet[2]);
#if APPLESDK
	clock_get_uptime(&now);
#else 
	clock_get_uptime((uint64_t*)&now);
#endif
    dispatchRelativePointerEvent(dx, dy, buttons, now);

  if ( packetSize > 3 )
  {
    dispatchRelativePointerEvent(dx, dy, buttons, now);

    //
    // We treat the 4th byte in the packet as a 8-bit signed Z value.
    // There are mice that can report only 4-bits for the Z data, and
    // use two of the remaining 4 bits for buttons 4 and 5.
    // %%KCD - As it turns out, the valid range for scroll data from
    // PS2 mice is -8 to +7, thus the upper four bits are just a sign
    // bit.  If we just sign extend the lower four bits, the scroll
    // calculation works for normal scrollwheel mice and five button mice.
    dz = (SInt16)(((SInt8)(packet[3] << 4)) >> 4);
    if ( dz )
    {
      //
      // The Z counter is negative on an upwards scroll (away from the user),
      // and positive when scrolling downwards. Invert this before passing to
      // HID/CG.
      //
      dispatchScrollWheelEvent(-dz, 0, 0, now);
    }
  }
  else
  {
      dispatchRelativePointerEvent(dx, dy, buttons, now);
  }

    return;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ALPSMultiTouch::setIntelliMouseMode()
{
  UInt8 Byte1;

  //
  // Reset the mouse as per Linux dump
  //

  PS2Request * request = _device->allocateRequest();
  if ( !request ) return;
  request->commands[0].command = kPS2C_SendMouseCommandAndCompareAck;
  request->commands[0].inOrOut = kDP_Reset;                          // 0xFF
  request->commandsCount = 1;
  _device->submitRequestAndBlock(request);
  _device->freeRequest(request);

  //
  // Generate the special command sequence to enable the 'Intellimouse' mode.
  // The sequence is to set the sampling rate to 200, 100, then 80, at which
  // point the mouse will start sending 4 byte packets for mouse events and
  // return a mouse ID of 3.
  //

  request = _device->allocateRequest();
  if ( !request ) return;
  request->commands[0].command = kPS2C_SendMouseCommandAndCompareAck;
  request->commands[0].inOrOut = kDP_SetMouseSampleRate;              // 0xF3
  request->commands[1].command = kPS2C_SendMouseCommandAndCompareAck;
  request->commands[1].inOrOut = 200;                                 // 0xC8
  request->commands[2].command = kPS2C_SendMouseCommandAndCompareAck;
  request->commands[2].inOrOut = kDP_SetMouseSampleRate;              // 0xF3
  request->commands[3].command = kPS2C_SendMouseCommandAndCompareAck;
  request->commands[3].inOrOut = 100;                                 // 0x64
  request->commands[4].command = kPS2C_SendMouseCommandAndCompareAck;
  request->commands[4].inOrOut = kDP_SetMouseSampleRate;              // 0xF3
  request->commands[5].command = kPS2C_SendMouseCommandAndCompareAck;
  request->commands[5].inOrOut = 80;                                  // 0x50
  request->commandsCount = 6;
  _device->submitRequestAndBlock(request);
  _device->freeRequest(request);

  //
  // Pause before sending the next command after switching mouse mode.
  //

  IOSleep(50);

  //
  // Determine whether we have an Intellimouse by asking for the mouse's ID.
  //

  request = _device->allocateRequest();
  if ( !request ) return;
  request->commands[0].command = kPS2C_SendMouseCommandAndCompareAck;
  request->commands[0].inOrOut = kDP_GetId;                          // 0xF2
  request->commands[1].command = kPS2C_ReadDataPort;
  request->commands[1].inOrOut = 0;
  request->commandsCount = 2;
  _device->submitRequestAndBlock(request);
  Byte1 = request->commands[1].inOrOut;
  if (Byte1 == 0x03)  {DEBUG_LOG("IntelliMouse Mode Success!");}
  _device->freeRequest(request);

  //
  // Restore the original sampling rate, before we obliterated it.
  //

  setSampleRateAndResolution();

}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ALPSMultiTouch::setTapEnable( bool enable )
{
    //
    // Instructs the trackpad to honor or ignore tapping
    //
	ALPSStatus_t Status;
	bool success;
	
    PS2Request * request = _device->allocateRequest();
    if ( !request ) return;

    request->commands[0].command  = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[0].inOrOut  = kDP_SetMouseScaling2To1;
    request->commands[1].command  = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[1].inOrOut  = kDP_SetMouseScaling2To1;
    request->commands[2].command  = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[2].inOrOut  = kDP_SetMouseScaling2To1;
    request->commands[3].command  = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[3].inOrOut  = kDP_SetDefaultsAndDisable;
    request->commands[4].command  = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[4].inOrOut  = kDP_GetMouseInformation;
    request->commands[5].command  = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[5].inOrOut  = kDP_SetDefaultsAndDisable;
    request->commands[6].command  = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[6].inOrOut  = kDP_SetDefaultsAndDisable;

	getStatus(&Status);
	if (Status.Byte1 & 0x04) 
    {
        DEBUG_LOG("Tapping can only be toggled.\n");
		enable = false;
	}

    if (enable)
	{
		request->commands[7].command = kPS2C_SendMouseCommandAndCompareAck;
    	request->commands[7].inOrOut = kDP_SetMouseSampleRate;
		request->commands[8].command = kPS2C_SendMouseCommandAndCompareAck;
    	request->commands[8].inOrOut = 0x0A;
	}
	else
	{
		request->commands[7].command = kPS2C_SendMouseCommandAndCompareAck;
    	request->commands[7].inOrOut = kDP_SetMouseResolution;
		request->commands[8].command = kPS2C_SendMouseCommandAndCompareAck;
    	request->commands[8].inOrOut = 0x00;
	}

    request->commands[9].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[9].inOrOut = kDP_SetMouseScaling1To1;
    request->commands[10].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[10].inOrOut = kDP_SetMouseScaling1To1;
    request->commands[11].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[11].inOrOut = kDP_SetMouseScaling1To1;
    request->commands[12].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[12].inOrOut = kDP_SetDefaultsAndDisable;
    request->commands[13].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[13].inOrOut = kDP_Enable;
    request->commandsCount = 14;
	_device->submitRequestAndBlock(request);

	getStatus(&Status);

    success = (request->commandsCount == 14);
	if (success)
	{
		setSampleRateAndResolution();
	}

    _device->freeRequest(request);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ALPSMultiTouch::setTouchPadEnable( bool enable )
{
    //
    // Instructs the trackpad to start or stop the reporting of data packets.
    // It is safe to issue this request from the interrupt/completion context.
    //

    PS2Request * request = _device->allocateRequest();
    if ( !request ) return;

    // (mouse enable/disable command)
    request->commands[0].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[0].inOrOut = kDP_SetDefaultsAndDisable;
    request->commands[1].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[1].inOrOut = kDP_SetDefaultsAndDisable;
    request->commands[2].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[2].inOrOut = kDP_SetDefaultsAndDisable;
    request->commands[3].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[3].inOrOut = kDP_SetDefaultsAndDisable;

	// (mouse or pad enable/disable command)
    request->commands[4].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[4].inOrOut = (enable)?kDP_Enable:kDP_SetDefaultsAndDisable;
    request->commandsCount = 5;
    _device->submitRequest(request); // asynchronous, auto-free'd
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ALPSMultiTouch::setSampleRateAndResolution( void )
{
    PS2Request * request = _device->allocateRequest();
    if ( !request ) return;
    request->commands[0].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[0].inOrOut = kDP_SetDefaultsAndDisable;           // 0xF5, Disable data reporting 
    request->commands[1].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[1].inOrOut = kDP_SetMouseSampleRate;              // 0xF3
    request->commands[2].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[2].inOrOut = 0x64;                                // 100 dpi
    request->commands[3].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[3].inOrOut = kDP_SetMouseResolution;              // 0xE8
    request->commands[4].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[4].inOrOut = 0x03;                                // 0x03 = 8 counts/mm
    request->commands[5].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[5].inOrOut = kDP_SetMouseScaling1To1;             // 0xE6
    request->commands[6].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[6].inOrOut = kDP_Enable;                          // 0xF4, Enable Data Reporting
    request->commandsCount = 7;
    _device->submitRequestAndBlock(request);
    _device->freeRequest(request);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ALPSMultiTouch::setCommandByte( UInt8 setBits, UInt8 clearBits )
{
    //
    // Sets the bits setBits and clears the bits clearBits "atomically" in the
    // controller's Command Byte.   Since the controller does not provide such
    // a read-modify-write primitive, we resort to a test-and-set try loop.
    //
    // Do NOT issue this request from the interrupt/completion context.
    //

    UInt8        commandByte;
    UInt8        commandByteNew;
    PS2Request * request = _device->allocateRequest();

    if ( !request ) return;

    do
    {
        // (read command byte)
        request->commands[0].command = kPS2C_WriteCommandPort;
        request->commands[0].inOrOut = kCP_GetCommandByte;
        request->commands[1].command = kPS2C_ReadDataPort;
        request->commands[1].inOrOut = 0;
        request->commandsCount = 2;
        _device->submitRequestAndBlock(request);

        //
        // Modify the command byte as requested by caller.
        //

        commandByte    = request->commands[1].inOrOut;
        commandByteNew = (commandByte | setBits) & (~clearBits);

        // ("test-and-set" command byte)
        request->commands[0].command = kPS2C_WriteCommandPort;
        request->commands[0].inOrOut = kCP_GetCommandByte;
        request->commands[1].command = kPS2C_ReadDataPortAndCompare;
        request->commands[1].inOrOut = commandByte;
        request->commands[2].command = kPS2C_WriteCommandPort;
        request->commands[2].inOrOut = kCP_SetCommandByte;
        request->commands[3].command = kPS2C_WriteDataPort;
        request->commands[3].inOrOut = commandByteNew;
        request->commandsCount = 4;
        _device->submitRequestAndBlock(request);

        //
        // Repeat this loop if last command failed, that is, if the
        // old command byte was modified since we first read it.
        //

    } while (request->commandsCount != 4);  

    _device->freeRequest(request);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

IOReturn ApplePS2ALPSMultiTouch::setParamProperties( OSDictionary * dict )
{
    OSNumber * clicking = OSDynamicCast( OSNumber, dict->getObject("Clicking") );
	OSNumber * dragging = OSDynamicCast( OSNumber, dict->getObject("Dragging") );
	OSNumber * draglock = OSDynamicCast( OSNumber, dict->getObject("DragLock") );
    OSNumber * hscroll  = OSDynamicCast( OSNumber, dict->getObject("TrackpadHorizScroll") );
    OSNumber * vscroll  = OSDynamicCast( OSNumber, dict->getObject("TrackpadScroll") );
    OSNumber * eaccell  = OSDynamicCast( OSNumber, dict->getObject("HIDTrackpadScrollAcceleration") );
	OSNumber * accell   = OSDynamicCast( OSNumber, dict->getObject("HIDTrackpadAcceleration") );

	dict->removeObject("HIDPointerAcceleration");

    OSCollectionIterator* iter = OSCollectionIterator::withCollection( dict );
    OSObject* obj;
    
    iter->reset();
    while ((obj = iter->getNextObject()) != NULL)
    {
        OSString* str = OSDynamicCast( OSString, obj );
        OSNumber* val = OSDynamicCast( OSNumber, dict->getObject( str ) );
        
        if (val){
            //DEBUG_LOG("Dictionary Object: %s Value: %d\n", 
                //str->getCStringNoCopy(), val->unsigned32BitValue());
		}
        else {
            //DEBUG_LOG("Dictionary Object: %s Value: ??\n", 
                //str->getCStringNoCopy());
		}
    }

    if ( clicking )
    {    
        UInt8  newModeByteValue = clicking->unsigned32BitValue() & 0x1 ?
                                  kTapEnabled :
                                  0;

        if (!TapSettingsLoaded) {
			DEBUG_LOG(" ABmod, Loading Clicking Settings at Boot: %d, %d, %d", clicking->unsigned32BitValue() ,kTapEnabled,newModeByteValue);
		}
		if ((_touchPadModeByte != newModeByteValue) || (!TapSettingsLoaded))
        {
            _touchPadModeByte = newModeByteValue;
			setTapEnable(_touchPadModeByte);
			setProperty("Clicking", clicking);
			if(_packetByteCount == 6)
			{
				setAbsoluteMode(); //restart the mouse...
			}
			
			TapSettingsLoaded = true;
        }
    }

	if (accell) {
		setProperty("HIDTrackpadAcceleration", accell->unsigned32BitValue());
	}

	if (dragging)
	{
		_dragging = dragging->unsigned32BitValue() & 0x1 ? true : false;
		setProperty("Dragging", dragging);
	}

	if (draglock)
	{
		_draglock = draglock->unsigned32BitValue() & 0x1 ? true : false;
		setProperty("DragLock", draglock);
	}

    if (hscroll)
    {
        _edgehscroll = hscroll->unsigned32BitValue() & 0x1 ? true : false;
        setProperty("TrackpadHorizScroll", hscroll);
    }

    if (vscroll)
    {
        _edgevscroll = vscroll->unsigned32BitValue() & 0x1 ? true : false;
        setProperty("TrackpadScroll", vscroll);
        }
    if (eaccell)
    {
        _edgeaccell = eaccell->unsigned32BitValue();
        _edgeaccellvalue = (((double)(_edgeaccell / 1966.08)) / 375.0);  //Slice was 75 - too fast
        _edgeaccellvalue = _edgeaccellvalue == 0 ? 0.01 : _edgeaccellvalue;
        setProperty("HIDTrackpadScrollAcceleration", eaccell);
    }

    return super::setParamProperties(dict);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ALPSMultiTouch::setDevicePowerState( UInt32 whatToDo )
{
    switch ( whatToDo )
    {
        case kPS2C_DisableDevice:
            
            //
            // Disable touchpad.
            //

            setTouchPadEnable( false );
            break;

        case kPS2C_EnableDevice:
		case 2:  //Slice :)
			
			IOSleep(1000);

            setTapEnable( _touchPadModeByte );


            //
            // Enable the mouse clock (should already be so) and the
            // mouse IRQ line.
            //

            setCommandByte( kCB_EnableMouseIRQ, kCB_DisableMouseClock );

			DEBUG_LOG(" ABMod Waking up Touchpad setting setTapEnable to %d\n",_touchPadModeByte);

			//
			//This is the way devs hit things when they don't work
			//Hit false / Hit true - OK now we have a picture good sit back and relax- ab_73
			//					
			setTouchPadEnable( true ); 
		
			setTouchPadEnable(false);
			setTouchPadEnable(true);

			//
            // Finally, we enable the trackpad itself, so that it may
            // start reporting asynchronous events.
            //
			
			setTapEnable( _touchPadModeByte );
			if(_packetByteCount == 6)
			{
				setAbsoluteMode();
			}
            //setTouchPadEnable( true );
            break;
	}
}

// - - - - - - - - -- - - - - - - - - - - - -- - - - - - - - -- - - - - - - -

void ApplePS2ALPSMultiTouch::setAbsoluteMode()
{
    PS2Request * request = _device->allocateRequest();
    if ( !request ) return;

    // Try ALPS magic knock - 4 disable before enable
    request->commands[0].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[0].inOrOut = kDP_SetDefaultsAndDisable;           //F5
    request->commands[1].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[1].inOrOut = kDP_SetDefaultsAndDisable;           //F5
    request->commands[2].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[2].inOrOut = kDP_SetDefaultsAndDisable;           //F5
    request->commands[3].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[3].inOrOut = kDP_SetDefaultsAndDisable;           //F5
    request->commands[4].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[4].inOrOut = kDP_Enable;                          //F4

    // Switch mouse to poll (remote) mode so motion data will not get in our way

    request->commands[5].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[5].inOrOut = kDP_SetMousePoll;                    //F0
    request->commandsCount = 6;
    _device->submitRequestAndBlock(request);
    _device->freeRequest(request);
}

// - - - - - - - - -- - - - - - - - - - - - -- - - - - - - - -- - - - - - - -

bool ApplePS2ALPSMultiTouch::setECMode() // Enable register access mode
{
    UInt8   Byte1, Byte2, Byte3;
    PS2Request * request = _device->allocateRequest();

    if (!request) return false;

    // Andrew Skalski said that 0xEA it's not strictly required, i'll do it anyway
	// (EA) EC EC EC E9
	// To escape from register access mode send 0xEA  command
    request->commands[0].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[0].inOrOut = kDP_SetMouseStreamMode;              // 0xEA
    request->commandsCount = 1;
    _device->submitRequestAndBlock(request);
    _device->freeRequest(request);

    request = _device->allocateRequest();
    if (!request) return false;
    // Set "EC" Mode
    request->commands[0].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[0].inOrOut = kDP_MouseResetWrap;                  // 0xEC
    request->commands[1].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[1].inOrOut = kDP_MouseResetWrap;                  // 0xEC
    request->commands[2].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[2].inOrOut = kDP_MouseResetWrap;                  // 0xEC
    request->commandsCount = 3;
    _device->submitRequestAndBlock(request);
    _device->freeRequest(request);

    request = _device->allocateRequest();
    if (!request) return false;
    // Result is "EC Report"
    request->commands[0].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[0].inOrOut = kDP_GetMouseInformation;             // 0xE9
    request->commands[1].command = kPS2C_ReadDataPort;
    request->commands[1].inOrOut = 0xFF;
    request->commands[2].command = kPS2C_ReadDataPort;
    request->commands[2].inOrOut = 0xFF;
    request->commands[3].command = kPS2C_ReadDataPort;
    request->commands[3].inOrOut = 0xFF;
    request->commandsCount = 4;
    _device->submitRequestAndBlock(request);

    Byte1 = request->commands[1].inOrOut;
    Byte2 = request->commands[2].inOrOut;
    Byte3 = request->commands[3].inOrOut;
    _device->freeRequest(request);
    DEBUG_LOG("EC Report: { 0x%02x, 0x%02x, 0x%02x }\n",
        Byte1, Byte2, Byte3);

    if (Byte1 != 0x88 || Byte2 != 0x07 || (Byte3 != 0x9b && Byte3 != 0x9d)) // No luck so far :(
	{
		DEBUG_LOG("Failed to enter EC Mode!\n");
		return false;
	}
	else
	{
		DEBUG_LOG("EC Mode Success!\n");
		return true;
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ALPSMultiTouch::setMisc( UInt16 val )
{
    PS2Request * request = _device->allocateRequest();
    if ( !request ) return;
    int i = 0;
    AlpsECNibble(request, &i, val >> 4);
    AlpsECNibble(request, &i, val);
    request->commandsCount = 2;
    _device->submitRequest(request); 
    DEBUG_LOG("setMisc: { 0x%02x, 0x%02x}\n", val >> 4, val && 0xF);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ALPSMultiTouch::AlpsECNibble(PS2Request * request, int * index, uint8_t nibble)
{
    unsigned char param;
    static const int cmds[] = {
        kDP_SetMousePoll,           // 0xF0
        kDP_SetDefaults,            // 0xF6
        kDP_SetMouseScaling2To1,    // 0xE7
        kDP_SetMouseSampleRate,     // 0xF3
        kDP_SetMouseSampleRate,     // 0xF3
        kDP_SetMouseSampleRate,     // 0xF3
        kDP_SetMouseSampleRate,     // 0xF3
        kDP_SetMouseSampleRate,     // 0xF3
        kDP_SetMouseSampleRate,     // 0xF3
        kDP_SetMouseSampleRate,     // 0xF3
        kDP_GetMouseInformation,    // 0xE9
        kDP_SetMouseResolution,     // 0xE8
        kDP_SetMouseResolution,     // 0xE8
        kDP_SetMouseResolution,     // 0xE8
        kDP_SetMouseResolution,     // 0xE8
        kDP_SetMouseScaling1To1 };  // 0xE6

    static const unsigned char params[] = {
        0xff, 0xff, 0xff, 10, 20, 40, 60, 80, 100, 200, 0xff, 0, 1, 2, 3, 0xff };

    nibble &= 0xf;
    request->commands[*index].command  = kPS2C_SendMouseCommandAndCompareAck;  
    request->commands[(*index)++].inOrOut =  cmds[nibble]; 
    DEBUG_LOG("EC Nibble: index:%d cmd:%x param:%x\n", *index, cmds[nibble], params[nibble]);
    
	param = params[nibble];
    if (param != 0xFF)
    {
        request->commands[*index].command  = kPS2C_SendMouseCommandAndCompareAck;  
        request->commands[(*index)++].inOrOut = param; 
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

int ApplePS2ALPSMultiTouch::AlpsECWrite(uint16_t addr, uint8_t value)
{
    UInt16 AddrH, AddrL;
    UInt8 Val;
    PS2Request * request = _device->allocateRequest();
    
    if ( !request ) return 0;

    // Select new address: EC addr3 addr2 addr1 addr0 (nibble3 nibble2 nibble1 nibble0)
    request->commands[0].command  = kPS2C_SendMouseCommandAndCompareAck;  //4
    request->commands[0].inOrOut =  kDP_MouseResetWrap; //sync.. EC

    int index = 1;
    AlpsECNibble(request, &index, addr >> 12);
    AlpsECNibble(request, &index, addr >> 8);
    AlpsECNibble(request, &index, addr >> 4);
    AlpsECNibble(request, &index, addr);

    // 0xE9 Read byte from current register, reply: 2-byte address + 1-byte data,
    // { addr_high, addr_low, value }
    request->commands[index].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[index++].inOrOut = kDP_GetMouseInformation;
    int indRead = index;
    request->commands[index].command  = kPS2C_ReadDataPort;
    request->commands[index++].inOrOut  = 0;
    request->commands[index].command = kPS2C_ReadDataPort;
    request->commands[index++].inOrOut = 0;
    request->commands[index].command = kPS2C_ReadDataPort;
    request->commands[index++].inOrOut = 0;
    // Write byte: value1 value0
    if (value) {
        AlpsECNibble(request, &index, value >> 4);
        AlpsECNibble(request, &index, value);
    }

    request->commandsCount = index;
    _device->submitRequestAndBlock(request);

    AddrH = request->commands[indRead++].inOrOut;
    AddrL = request->commands[indRead++].inOrOut;
    Val = request->commands[indRead].inOrOut;

    DEBUG_LOG("EC Write Response: { addr_high: 0x%04x, addr_low: 0x%04x, value: 0x%02x }\n",
        AddrH, AddrL, Val);

    _device->freeRequest(request);
    return 0;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ALPSMultiTouch::getMouseInformation()
{
	UInt8 Byte1, Byte2, Byte3;
    PS2Request * request = _device->allocateRequest();

    if ( !request ) return;
    request->commands[0].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[0].inOrOut = kDP_GetMouseInformation;
    request->commands[1].command = kPS2C_ReadDataPort;
    request->commands[1].inOrOut = 0;
    request->commands[2].command = kPS2C_ReadDataPort;
    request->commands[2].inOrOut = 0;
    request->commands[3].command = kPS2C_ReadDataPort;
    request->commands[3].inOrOut = 0;
    request->commandsCount = 4;
    _device->submitRequestAndBlock(request);
	Byte1 = request->commands[1].inOrOut;
	Byte2 = request->commands[2].inOrOut;
	Byte3 = request->commands[3].inOrOut;
    DEBUG_LOG("getMouseInformation(): { 0x%02x, 0x%02x, 0x%02x }\n", Byte1, Byte2, Byte3);
	_device->freeRequest(request);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ALPSMultiTouch::getStatus(ALPSStatus_t *status)
{
    PS2Request * request = _device->allocateRequest();

    if ( !request ) return;
    request->commands[0].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[0].inOrOut = kDP_SetDefaultsAndDisable;
    request->commands[1].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[1].inOrOut = kDP_SetDefaultsAndDisable;
    request->commands[2].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[2].inOrOut = kDP_SetDefaultsAndDisable;
    request->commands[3].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[3].inOrOut = kDP_GetMouseInformation;
    request->commands[4].command = kPS2C_ReadDataPort;
    request->commands[4].inOrOut = 0;
    request->commands[5].command = kPS2C_ReadDataPort;
    request->commands[5].inOrOut = 0;
    request->commands[6].command = kPS2C_ReadDataPort;
    request->commands[6].inOrOut = 0;
    request->commandsCount = 7;
    _device->submitRequestAndBlock(request);
    status->Byte1 = request->commands[4].inOrOut;
    status->Byte2 = request->commands[5].inOrOut;
    status->Byte3 = request->commands[6].inOrOut;
    DEBUG_LOG("getStatus(): { 0x%02x, 0x%02x, 0x%02x }\n", status->Byte1, status->Byte2, status->Byte3);
    _device->freeRequest(request);
}

// =============================================================================