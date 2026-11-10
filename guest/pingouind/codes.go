package main

// linux/input-event-codes.h
const (
	evSyn = 0x00
	evKey = 0x01
	evAbs = 0x03

	synReport = 0

	btnLeft          = 0x110
	btnToolFinger    = 0x145
	btnToolQuinttap  = 0x148
	btnTouch         = 0x14a
	btnToolDoubletap = 0x14d
	btnToolTripletap = 0x14e
	btnToolQuadtap   = 0x14f

	absX            = 0x00
	absY            = 0x01
	absMtSlot       = 0x2f
	absMtPositionX  = 0x35
	absMtPositionY  = 0x36
	absMtTrackingID = 0x39

	inputPropPointer   = 0x00
	inputPropButtonpad = 0x02

	busUSB = 0x03
)
