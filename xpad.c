/*
 * X-Box gamepad driver
 *
 * Copyright (c) 2002 Marko Friedemann <mfr@bmx-chemnitz.de>
 *               2004 Oliver Schwartz <Oliver.Schwartz@gmx.de>,
 *                    Steven Toth <steve@toth.demon.co.uk>,
 *                    Franz Lehner <franz@caos.at>,
 *                    Ivan Hawkes <blackhawk@ivanhawkes.com>
 *               2005 Dominic Cerquetti <binary1230@yahoo.com>
 *               2006 Adam Buchbinder <adam.buchbinder@gmail.com>
 *               2007 Jan Kratochvil <honza@jikos.cz>
 *               2010 Christoph Fritz <chf.fritz@googlemail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 *
 * This driver is based on:
 *  - information from     http://euc.jp/periphs/xbox-controller.ja.html
 *  - the iForce driver    drivers/char/joystick/iforce.c
 *  - the skeleton-driver  drivers/usb/usb-skeleton.c
 *  - Xbox 360 information http://www.free60.org/wiki/Gamepad
 *  - Xbox One information https://github.com/quantus/xbox-one-controller-protocol
 *
 * Thanks to:
 *  - ITO Takayuki for providing essential xpad information on his website
 *  - Vojtech Pavlik     - iforce driver / input subsystem
 *  - Greg Kroah-Hartman - usb-skeleton driver
 *  - XBOX Linux project - extra USB id's
 *  - Pekka Pöyry (quantus) - Xbox One controller reverse engineering
 *
 * TODO:
 *  - fine tune axes (especially trigger axes)
 *  - fix "analog" buttons (reported as digital now)
 *  - get rumble working
 *  - need USB IDs for other dance pads
 *
 * History:
 *
 * 2002-06-27 - 0.0.1 : first version, just said "XBOX HID controller"
 *
 * 2002-07-02 - 0.0.2 : basic working version
 *  - all axes and 9 of the 10 buttons work (german InterAct device)
 *  - the black button does not work
 *
 * 2002-07-14 - 0.0.3 : rework by Vojtech Pavlik
 *  - indentation fixes
 *  - usb + input init sequence fixes
 *
 * 2002-07-16 - 0.0.4 : minor changes, merge with Vojtech's v0.0.3
 *  - verified the lack of HID and report descriptors
 *  - verified that ALL buttons WORK
 *  - fixed d-pad to axes mapping
 *
 * 2002-07-17 - 0.0.5 : simplified d-pad handling
 *
 * 2004-10-02 - 0.0.6 : DDR pad support
 *  - borrowed from the XBOX linux kernel
 *  - USB id's for commonly used dance pads are present
 *  - dance pads will map D-PAD to buttons, not axes
 *  - pass the module paramater 'dpad_to_buttons' to force
 *    the D-PAD to map to buttons if your pad is not detected
 *
 * Later changes can be tracked in SCM.
 */
// #define DEBUG
#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/module.h>
#include <linux/usb/input.h>
#include <linux/usb/quirks.h>

#define XPAD_PKT_LEN 64
#define XPAD_PKT_LEN_GUNCON3 8
#define XPAD_PKT_LEN_GUNCON3_IN 15

/*
 * xbox d-pads should map to buttons, as is required for DDR pads
 * but we map them to axes when possible to simplify things
 */
#define MAP_DPAD_TO_BUTTONS		(1 << 0)
#define MAP_TRIGGERS_TO_BUTTONS		(1 << 1)
#define MAP_STICKS_TO_NULL		(1 << 2)
#define DANCEPAD_MAP_CONFIG	(MAP_DPAD_TO_BUTTONS |			\
				MAP_TRIGGERS_TO_BUTTONS | MAP_STICKS_TO_NULL)

#define XTYPE_XBOX        0
#define XTYPE_XBOX360     1
#define XTYPE_XBOX360W    2
#define XTYPE_XBOXONE     3
#define XTYPE_UNKNOWN     4
#define XTYPE_GUNCON3     5


static bool dpad_to_buttons;
module_param(dpad_to_buttons, bool, S_IRUGO);
MODULE_PARM_DESC(dpad_to_buttons, "Map D-PAD to buttons rather than axes for unknown pads");

static bool triggers_to_buttons;
module_param(triggers_to_buttons, bool, S_IRUGO);
MODULE_PARM_DESC(triggers_to_buttons, "Map triggers to buttons rather than axes for unknown pads");

static bool sticks_to_null;
module_param(sticks_to_null, bool, S_IRUGO);
MODULE_PARM_DESC(sticks_to_null, "Do not map sticks at all for unknown pads");

static bool auto_poweroff = true;
module_param(auto_poweroff, bool, S_IWUSR | S_IRUGO);
MODULE_PARM_DESC(auto_poweroff, "Power off wireless controllers on suspend");

static const struct xpad_device {
	u16 idVendor;
	u16 idProduct;
	char *name;
	u8 mapping;
	u8 xtype;
} xpad_device[] = {
	{ 0x0079, 0x18d4, "GPD Win 2 X-Box Controller", 0, XTYPE_XBOX360 },
    { 0x044f, 0x0f00, "Thrustmaster Wheel", 0, XTYPE_XBOX },
    { 0x044f, 0x0f03, "Thrustmaster Wheel", 0, XTYPE_XBOX },
    { 0x044f, 0x0f07, "Thrustmaster, Inc. Controller", 0, XTYPE_XBOX },
    { 0x044f, 0x0f10, "Thrustmaster Modena GT Wheel", 0, XTYPE_XBOX },
    { 0x044f, 0xb326, "Thrustmaster Gamepad GP XID", 0, XTYPE_XBOX360 },
    { 0x045e, 0x0202, "Microsoft X-Box pad v1 (US)", 0, XTYPE_XBOX },
    { 0x045e, 0x0285, "Microsoft X-Box pad (Japan)", 0, XTYPE_XBOX },
    { 0x045e, 0x0287, "Microsoft Xbox Controller S", 0, XTYPE_XBOX },
    { 0x045e, 0x0288, "Microsoft Xbox Controller S v2", 0, XTYPE_XBOX },
    { 0x045e, 0x0289, "Microsoft X-Box pad v2 (US)", 0, XTYPE_XBOX },
    { 0x045e, 0x028e, "Microsoft X-Box 360 pad", 0, XTYPE_XBOX360 },
    { 0x045e, 0x028f, "Microsoft X-Box 360 pad v2", 0, XTYPE_XBOX360 },
    { 0x045e, 0x0291, "Xbox 360 Wireless Receiver (XBOX)", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX360W },
    { 0x045e, 0x02d1, "Microsoft X-Box One pad", 0, XTYPE_XBOXONE },
    { 0x045e, 0x02dd, "Microsoft X-Box One pad (Firmware 2015)", 0, XTYPE_XBOXONE },
    { 0x045e, 0x02e3, "Microsoft X-Box One Elite pad", 0, XTYPE_XBOXONE },
    { 0x045e, 0x02ea, "Microsoft X-Box One S pad", 0, XTYPE_XBOXONE },
    { 0x2e24, 0x0652, "Hyperkin Duke X-Box One pad", 0, XTYPE_XBOXONE },
    { 0x045e, 0x0719, "Xbox 360 Wireless Receiver", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX360W },
    { 0x046d, 0xc21d, "Logitech Gamepad F310", 0, XTYPE_XBOX360 },
    { 0x046d, 0xc21e, "Logitech Gamepad F510", 0, XTYPE_XBOX360 },
    { 0x046d, 0xc21f, "Logitech Gamepad F710", 0, XTYPE_XBOX360 },
    { 0x046d, 0xc242, "Logitech Chillstream Controller", 0, XTYPE_XBOX360 },
    { 0x046d, 0xca84, "Logitech Xbox Cordless Controller", 0, XTYPE_XBOX },
    { 0x046d, 0xca88, "Logitech Compact Controller for Xbox", 0, XTYPE_XBOX },
    { 0x046d, 0xca8a, "Logitech Precision Vibration Feedback Wheel", 0, XTYPE_XBOX },
    { 0x046d, 0xcaa3, "Logitech DriveFx Racing Wheel", 0, XTYPE_XBOX360 },
    { 0x056e, 0x2004, "Elecom JC-U3613M", 0, XTYPE_XBOX360 },
    { 0x05fd, 0x1007, "Mad Catz Controller (unverified)", 0, XTYPE_XBOX },
    { 0x05fd, 0x107a, "InterAct 'PowerPad Pro' X-Box pad (Germany)", 0, XTYPE_XBOX },
    { 0x05fe, 0x3030, "Chic Controller", 0, XTYPE_XBOX },
    { 0x05fe, 0x3031, "Chic Controller", 0, XTYPE_XBOX },
    { 0x062a, 0x0020, "Logic3 Xbox GamePad", 0, XTYPE_XBOX },
    { 0x062a, 0x0033, "Competition Pro Steering Wheel", 0, XTYPE_XBOX },
    { 0x06a3, 0x0200, "Saitek Racing Wheel", 0, XTYPE_XBOX },
    { 0x06a3, 0x0201, "Saitek Adrenalin", 0, XTYPE_XBOX },
    { 0x06a3, 0xf51a, "Saitek P3600", 0, XTYPE_XBOX360 },
    { 0x0738, 0x4506, "Mad Catz 4506 Wireless Controller", 0, XTYPE_XBOX },
    { 0x0738, 0x4516, "Mad Catz Control Pad", 0, XTYPE_XBOX },
    { 0x0738, 0x4520, "Mad Catz Control Pad Pro", 0, XTYPE_XBOX },
    { 0x0738, 0x4522, "Mad Catz LumiCON", 0, XTYPE_XBOX },
    { 0x0738, 0x4526, "Mad Catz Control Pad Pro", 0, XTYPE_XBOX },
    { 0x0738, 0x4530, "Mad Catz Universal MC2 Racing Wheel and Pedals", 0, XTYPE_XBOX },
    { 0x0738, 0x4536, "Mad Catz MicroCON", 0, XTYPE_XBOX },
    { 0x0738, 0x4540, "Mad Catz Beat Pad", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
    { 0x0738, 0x4556, "Mad Catz Lynx Wireless Controller", 0, XTYPE_XBOX },
    { 0x0738, 0x4586, "Mad Catz MicroCon Wireless Controller", 0, XTYPE_XBOX },
    { 0x0738, 0x4588, "Mad Catz Blaster", 0, XTYPE_XBOX },
    { 0x0738, 0x45ff, "Mad Catz Beat Pad (w/ Handle)", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
    { 0x0738, 0x4716, "Mad Catz Wired Xbox 360 Controller", 0, XTYPE_XBOX360 },
    { 0x0738, 0x4718, "Mad Catz Street Fighter IV FightStick SE", 0, XTYPE_XBOX360 },
    { 0x0738, 0x4726, "Mad Catz Xbox 360 Controller", 0, XTYPE_XBOX360 },
    { 0x0738, 0x4728, "Mad Catz Street Fighter IV FightPad", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x0738, 0x4736, "Mad Catz MicroCon Gamepad", 0, XTYPE_XBOX360 },
    { 0x0738, 0x4738, "Mad Catz Wired Xbox 360 Controller (SFIV)", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x0738, 0x4740, "Mad Catz Beat Pad", 0, XTYPE_XBOX360 },
    { 0x0738, 0x4743, "Mad Catz Beat Pad Pro", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
    { 0x0738, 0x4758, "Mad Catz Arcade Game Stick", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x0738, 0x4a01, "Mad Catz FightStick TE 2", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOXONE },
    { 0x0738, 0x6040, "Mad Catz Beat Pad Pro", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
    { 0x0738, 0x9871, "Mad Catz Portable Drum", 0, XTYPE_XBOX360 },
    { 0x0738, 0xb726, "Mad Catz Xbox controller - MW2", 0, XTYPE_XBOX360 },
    { 0x0738, 0xb738, "Mad Catz MVC2TE Stick 2", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x0738, 0xbeef, "Mad Catz JOYTECH NEO SE Advanced GamePad", XTYPE_XBOX360 },
    { 0x0738, 0xcb02, "Saitek Cyborg Rumble Pad - PC/Xbox 360", 0, XTYPE_XBOX360 },
    { 0x0738, 0xcb03, "Saitek P3200 Rumble Pad - PC/Xbox 360", 0, XTYPE_XBOX360 },
    { 0x0738, 0xcb29, "Saitek Aviator Stick AV8R02", 0, XTYPE_XBOX360 },
    { 0x0738, 0xf738, "Super SFIV FightStick TE S", 0, XTYPE_XBOX360 },
    { 0x07ff, 0xffff, "Mad Catz GamePad", 0, XTYPE_XBOX360 },
    { 0x0c12, 0x0005, "Intec wireless", 0, XTYPE_XBOX },
    { 0x0c12, 0x8801, "Nyko Xbox Controller", 0, XTYPE_XBOX },
    { 0x0c12, 0x8802, "Zeroplus Xbox Controller", 0, XTYPE_XBOX },
    { 0x0c12, 0x8809, "RedOctane Xbox Dance Pad", DANCEPAD_MAP_CONFIG, XTYPE_XBOX },
    { 0x0c12, 0x880a, "Pelican Eclipse PL-2023", 0, XTYPE_XBOX },
    { 0x0c12, 0x8810, "Zeroplus Xbox Controller", 0, XTYPE_XBOX },
    { 0x0c12, 0x9902, "HAMA VibraX - *FAULTY HARDWARE*", 0, XTYPE_XBOX },
    { 0x0d2f, 0x0002, "Andamiro Pump It Up pad", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
    { 0x0e4c, 0x1097, "Radica Gamester Controller", 0, XTYPE_XBOX },
    { 0x0e4c, 0x1103, "Radica Gamester Reflex", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX },
    { 0x0e4c, 0x2390, "Radica Games Jtech Controller", 0, XTYPE_XBOX },
    { 0x0e4c, 0x3510, "Radica Gamester", 0, XTYPE_XBOX },
    { 0x0e6f, 0x0003, "Logic3 Freebird wireless Controller", 0, XTYPE_XBOX },
    { 0x0e6f, 0x0005, "Eclipse wireless Controller", 0, XTYPE_XBOX },
    { 0x0e6f, 0x0006, "Edge wireless Controller", 0, XTYPE_XBOX },
    { 0x0e6f, 0x0008, "After Glow Pro Controller", 0, XTYPE_XBOX },
    { 0x0e6f, 0x0105, "HSM3 Xbox360 dancepad", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x0e6f, 0x0113, "Afterglow AX.1 Gamepad for Xbox 360", 0, XTYPE_XBOX360 },
    { 0x0e6f, 0x011f, "Rock Candy Gamepad Wired Controller", 0, XTYPE_XBOX360 },
    { 0x0e6f, 0x0131, "PDP EA Sports Controller", 0, XTYPE_XBOX360 },
    { 0x0e6f, 0x0133, "Xbox 360 Wired Controller", 0, XTYPE_XBOX360 },
    { 0x0e6f, 0x0139, "Afterglow Prismatic Xbox One Wired Controller", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x02b3, "Afterglow Prismatic Xbox One Wired Controller", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x02b8, "Afterglow Prismatic Wired Controller for Xbox One", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x013a, "PDP Xbox One Controller", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x0146, "Rock Candy Wired Controller for Xbox One", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x0147, "PDP Marvel Xbox One Controller", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x015c, "PDP Xbox One Arcade Stick", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOXONE },
    { 0x0e6f, 0x0161, "PDP Xbox One Controller", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x0162, "PDP Xbox One Controller", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x0163, "PDP Xbox One Controller", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x02a0, "PDP Xbox One Controller", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x02a1, "PDP Xbox One Controller", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x02a7, "PDP Xbox One Controller", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x02a8, "PDP Xbox One Controller", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x0164, "PDP Battlefield One", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x0165, "PDP Titanfall 2", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x0201, "Pelican PL-3601 'TSZ' Wired Xbox 360 Controller", 0, XTYPE_XBOX360 },
    { 0x0e6f, 0x0213, "Afterglow Gamepad for Xbox 360", 0, XTYPE_XBOX360 },
    { 0x0e6f, 0x021f, "Rock Candy Gamepad for Xbox 360", 0, XTYPE_XBOX360 },
    { 0x0e6f, 0x0246, "Rock Candy Gamepad for Xbox One 2015", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x02ab, "PDP Controller for Xbox One", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x02a2, "PDP Wired Controller for Xbox One - Crimson Red", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x02a4, "PDP Wired Controller for Xbox One - Stealth Series", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x02a6, "PDP Wired Controller for Xbox One - Camo Series", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x02ad, "PDP Wired Controller for Xbox One - Stealth Series", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x0301, "Logic3 Controller", 0, XTYPE_XBOX360 },
    { 0x0e6f, 0x0346, "Rock Candy Gamepad for Xbox One 2016", 0, XTYPE_XBOXONE },
    { 0x0e6f, 0x0401, "Logic3 Controller", 0, XTYPE_XBOX360 },
    { 0x0e6f, 0x0413, "Afterglow AX.1 Gamepad for Xbox 360", 0, XTYPE_XBOX360 },
    { 0x0e6f, 0x0501, "PDP Xbox 360 Controller", 0, XTYPE_XBOX360 },
    { 0x0e6f, 0xf900, "PDP Afterglow AX.1", 0, XTYPE_XBOX360 },
    { 0x0e8f, 0x0201, "SmartJoy Frag Xpad/PS2 adaptor", 0, XTYPE_XBOX },
    { 0x0e8f, 0x3008, "Generic xbox control (dealextreme)", 0, XTYPE_XBOX },
    { 0x0f0d, 0x000a, "Hori Co. DOA4 FightStick", 0, XTYPE_XBOX360 },
    { 0x0f0d, 0x000c, "Hori PadEX Turbo", 0, XTYPE_XBOX360 },
    { 0x0f0d, 0x000d, "Hori Fighting Stick EX2", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x0f0d, 0x0016, "Hori Real Arcade Pro.EX", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x0f0d, 0x001b, "Hori Real Arcade Pro VX", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x0f0d, 0x0063, "Hori Real Arcade Pro Hayabusa (USA) Xbox One", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOXONE },
    { 0x0f0d, 0x0067, "HORIPAD ONE", 0, XTYPE_XBOXONE },
    { 0x0f0d, 0x0078, "Hori Real Arcade Pro V Kai Xbox One", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOXONE },
    { 0x0f30, 0x010b, "Philips Recoil", 0, XTYPE_XBOX },
    { 0x0f30, 0x0202, "Joytech Advanced Controller", 0, XTYPE_XBOX },
    { 0x0f30, 0x8888, "BigBen XBMiniPad Controller", 0, XTYPE_XBOX },
    { 0x102c, 0xff0c, "Joytech Wireless Advanced Controller", 0, XTYPE_XBOX },
    { 0x11c9, 0x55f0, "Nacon GC-100XF", 0, XTYPE_XBOX360 },
    { 0x12ab, 0x0004, "Honey Bee Xbox360 dancepad", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x12ab, 0x0301, "PDP AFTERGLOW AX.1", 0, XTYPE_XBOX360 },
    { 0x12ab, 0x0303, "Mortal Kombat Klassic FightStick", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x12ab, 0x8809, "Xbox DDR dancepad", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
    { 0x1430, 0x4748, "RedOctane Guitar Hero X-plorer", 0, XTYPE_XBOX360 },
    { 0x1430, 0x8888, "TX6500+ Dance Pad (first generation)", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
    { 0x1430, 0xf801, "RedOctane Controller", 0, XTYPE_XBOX360 },
    { 0x146b, 0x0601, "BigBen Interactive XBOX 360 Controller", 0, XTYPE_XBOX360 },
    { 0x1532, 0x0037, "Razer Sabertooth", 0, XTYPE_XBOX360 },
    { 0x1532, 0x0a00, "Razer Atrox Arcade Stick", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOXONE },
    { 0x1532, 0x0a03, "Razer Wildcat", 0, XTYPE_XBOXONE },
    { 0x15e4, 0x3f00, "Power A Mini Pro Elite", 0, XTYPE_XBOX360 },
    { 0x15e4, 0x3f0a, "Xbox Airflo wired controller", 0, XTYPE_XBOX360 },
    { 0x15e4, 0x3f10, "Batarang Xbox 360 controller", 0, XTYPE_XBOX360 },
    { 0x162e, 0xbeef, "Joytech Neo-Se Take2", 0, XTYPE_XBOX360 },
    { 0x1689, 0xfd00, "Razer Onza Tournament Edition", 0, XTYPE_XBOX360 },
    { 0x1689, 0xfd01, "Razer Onza Classic Edition", 0, XTYPE_XBOX360 },
    { 0x1689, 0xfe00, "Razer Sabertooth", 0, XTYPE_XBOX360 },
    { 0x1bad, 0x0002, "Harmonix Rock Band Guitar", 0, XTYPE_XBOX360 },
    { 0x1bad, 0x0003, "Harmonix Rock Band Drumkit", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x1bad, 0x0130, "Ion Drum Rocker", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x1bad, 0xf016, "Mad Catz Xbox 360 Controller", 0, XTYPE_XBOX360 },
    { 0x1bad, 0xf018, "Mad Catz Street Fighter IV SE Fighting Stick", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x1bad, 0xf019, "Mad Catz Brawlstick for Xbox 360", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x1bad, 0xf021, "Mad Cats Ghost Recon FS GamePad", 0, XTYPE_XBOX360 },
    { 0x1bad, 0xf023, "MLG Pro Circuit Controller (Xbox)", 0, XTYPE_XBOX360 },
    { 0x1bad, 0xf025, "Mad Catz Call Of Duty", 0, XTYPE_XBOX360 },
    { 0x1bad, 0xf027, "Mad Catz FPS Pro", 0, XTYPE_XBOX360 },
    { 0x1bad, 0xf028, "Street Fighter IV FightPad", 0, XTYPE_XBOX360 },
    { 0x1bad, 0xf02e, "Mad Catz Fightpad", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x1bad, 0xf030, "Mad Catz Xbox 360 MC2 MicroCon Racing Wheel", 0, XTYPE_XBOX360 },
    { 0x1bad, 0xf036, "Mad Catz MicroCon GamePad Pro", 0, XTYPE_XBOX360 },
    { 0x1bad, 0xf038, "Street Fighter IV FightStick TE", 0, XTYPE_XBOX360 },
    { 0x1bad, 0xf039, "Mad Catz MvC2 TE", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x1bad, 0xf03a, "Mad Catz SFxT Fightstick Pro", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x1bad, 0xf03d, "Street Fighter IV Arcade Stick TE - Chun Li", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x1bad, 0xf03e, "Mad Catz MLG FightStick TE", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x1bad, 0xf03f, "Mad Catz FightStick SoulCaliber", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x1bad, 0xf042, "Mad Catz FightStick TES+", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x1bad, 0xf080, "Mad Catz FightStick TE2", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x1bad, 0xf501, "HoriPad EX2 Turbo", 0, XTYPE_XBOX360 },
    { 0x1bad, 0xf502, "Hori Real Arcade Pro.VX SA", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x1bad, 0xf503, "Hori Fighting Stick VX", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x1bad, 0xf504, "Hori Real Arcade Pro. EX", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x1bad, 0xf505, "Hori Fighting Stick EX2B", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x1bad, 0xf506, "Hori Real Arcade Pro.EX Premium VLX", 0, XTYPE_XBOX360 },
    { 0x1bad, 0xf900, "Harmonix Xbox 360 Controller", 0, XTYPE_XBOX360 },
    { 0x1bad, 0xf901, "Gamestop Xbox 360 Controller", 0, XTYPE_XBOX360 },
    { 0x1bad, 0xf903, "Tron Xbox 360 controller", 0, XTYPE_XBOX360 },
    { 0x1bad, 0xf904, "PDP Versus Fighting Pad", 0, XTYPE_XBOX360 },
    { 0x1bad, 0xf906, "MortalKombat FightStick", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x1bad, 0xfa01, "MadCatz GamePad", 0, XTYPE_XBOX360 },
    { 0x1bad, 0xfd00, "Razer Onza TE", 0, XTYPE_XBOX360 },
    { 0x1bad, 0xfd01, "Razer Onza", 0, XTYPE_XBOX360 },
    { 0x24c6, 0x5000, "Razer Atrox Arcade Stick", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x24c6, 0x5300, "PowerA MINI PROEX Controller", 0, XTYPE_XBOX360 },
    { 0x24c6, 0x5303, "Xbox Airflo wired controller", 0, XTYPE_XBOX360 },
    { 0x24c6, 0x530a, "Xbox 360 Pro EX Controller", 0, XTYPE_XBOX360 },
    { 0x24c6, 0x531a, "PowerA Pro Ex", 0, XTYPE_XBOX360 },
    { 0x24c6, 0x5397, "FUS1ON Tournament Controller", 0, XTYPE_XBOX360 },
    { 0x24c6, 0x541a, "PowerA Xbox One Mini Wired Controller", 0, XTYPE_XBOXONE },
    { 0x24c6, 0x542a, "Xbox ONE spectra", 0, XTYPE_XBOXONE },
    { 0x24c6, 0x543a, "PowerA Xbox One wired controller", 0, XTYPE_XBOXONE },
    { 0x24c6, 0x5500, "Hori XBOX 360 EX 2 with Turbo", 0, XTYPE_XBOX360 },
    { 0x24c6, 0x5501, "Hori Real Arcade Pro VX-SA", 0, XTYPE_XBOX360 },
    { 0x24c6, 0x5502, "Hori Fighting Stick VX Alt", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x24c6, 0x5503, "Hori Fighting Edge", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x24c6, 0x5506, "Hori SOULCALIBUR V Stick", 0, XTYPE_XBOX360 },
    { 0x24c6, 0x550d, "Hori GEM Xbox controller", 0, XTYPE_XBOX360 },
    { 0x24c6, 0x550e, "Hori Real Arcade Pro V Kai 360", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
    { 0x24c6, 0x551a, "PowerA FUSION Pro Controller", 0, XTYPE_XBOXONE },
    { 0x24c6, 0x561a, "PowerA FUSION Controller", 0, XTYPE_XBOXONE },
    { 0x24c6, 0x5b00, "ThrustMaster Ferrari 458 Racing Wheel", 0, XTYPE_XBOX360 },
    { 0x24c6, 0x5b02, "Thrustmaster, Inc. GPX Controller", 0, XTYPE_XBOX360 },
    { 0x24c6, 0x5b03, "Thrustmaster Ferrari 458 Racing Wheel", 0, XTYPE_XBOX360 },
    { 0x24c6, 0x5d04, "Razer Sabertooth", 0, XTYPE_XBOX360 },
    { 0x24c6, 0xfafe, "Rock Candy Gamepad for Xbox 360", 0, XTYPE_XBOX360 },
    { 0x3767, 0x0101, "Fanatec Speedster 3 Forceshock Wheel", 0, XTYPE_XBOX },
    { 0xffff, 0xffff, "Chinese-made Xbox Controller", 0, XTYPE_XBOX },
    { 0x0b9a, 0x0800, "Guncon3 LigthGun",0,XTYPE_GUNCON3 },
    { 0x0000, 0x0000, "Generic X-Box pad", 0, XTYPE_UNKNOWN }
};

/* guncon3 buttons */
static const signed short guncon3_buttons[] = {
    BTN_TRIGGER,
    BTN_0, BTN_1,                   /* Buttons A1/A2 */
    BTN_2, BTN_3,                   /* Buttons B1/B2 */
    BTN_4, BTN_5,                   /* Buttons C1/C2 */
    BTN_6, BTN_7,                   /* Joystick buttons */
    -1
};

static const signed short guncon3_sticks[] = {
    ABS_X, ABS_Y,					/* Aiming */
    ABS_RX, ABS_RY,			/* Joystick A */
    ABS_HAT0X, ABS_HAT0Y,			/* Joystick B */
    -1
};

/* buttons shared with xbox and xbox360 */
static const signed short xpad_common_btn[] = {
	BTN_A, BTN_B, BTN_X, BTN_Y,			/* "analog" buttons */
	BTN_START, BTN_SELECT, BTN_THUMBL, BTN_THUMBR,	/* start/back/sticks */
	-1						/* terminating entry */
};

/* original xbox controllers only */
static const signed short xpad_btn[] = {
	BTN_C, BTN_Z,		/* "analog" buttons */
	-1			/* terminating entry */
};

/* used when dpad is mapped to buttons */
static const signed short xpad_btn_pad[] = {
	BTN_TRIGGER_HAPPY1, BTN_TRIGGER_HAPPY2,		/* d-pad left, right */
	BTN_TRIGGER_HAPPY3, BTN_TRIGGER_HAPPY4,		/* d-pad up, down */
	-1				/* terminating entry */
};

/* used when triggers are mapped to buttons */
static const signed short xpad_btn_triggers[] = {
	BTN_TL2, BTN_TR2,		/* triggers left/right */
	-1
};

static const signed short xpad360_btn[] = {  /* buttons for x360 controller */
	BTN_TL, BTN_TR,		/* Button LB/RB */
	BTN_MODE,		/* The big X button */
	-1
};

static const signed short xpad_abs[] = {
	ABS_X, ABS_Y,		/* left stick */
	ABS_RX, ABS_RY,		/* right stick */
	-1			/* terminating entry */
};

/* used when dpad is mapped to axes */
static const signed short xpad_abs_pad[] = {
	ABS_HAT0X, ABS_HAT0Y,	/* d-pad axes */
	-1			/* terminating entry */
};

/* used when triggers are mapped to axes */
static const signed short xpad_abs_triggers[] = {
	ABS_Z, ABS_RZ,		/* triggers left/right */
	-1
};

/*
 * Xbox 360 has a vendor-specific class, so we cannot match it with only
 * USB_INTERFACE_INFO (also specifically refused by USB subsystem), so we
 * match against vendor id as well. Wired Xbox 360 devices have protocol 1,
 * wireless controllers have protocol 129.
 */
#define XPAD_XBOX360_VENDOR_PROTOCOL(vend, pr) \
	.match_flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_INT_INFO, \
	.idVendor = (vend), \
	.bInterfaceClass = USB_CLASS_VENDOR_SPEC, \
	.bInterfaceSubClass = 93, \
	.bInterfaceProtocol = (pr)
#define XPAD_XBOX360_VENDOR(vend) \
	{ XPAD_XBOX360_VENDOR_PROTOCOL((vend), 1) }, \
	{ XPAD_XBOX360_VENDOR_PROTOCOL((vend), 129) }

/* The Xbox One controller uses subclass 71 and protocol 208. */
#define XPAD_XBOXONE_VENDOR_PROTOCOL(vend, pr) \
	.match_flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_INT_INFO, \
	.idVendor = (vend), \
	.bInterfaceClass = USB_CLASS_VENDOR_SPEC, \
	.bInterfaceSubClass = 71, \
	.bInterfaceProtocol = (pr)
#define XPAD_XBOXONE_VENDOR(vend) \
	{ XPAD_XBOXONE_VENDOR_PROTOCOL((vend), 208) }

/*Set guncon3 parametros */
#define XPAD_GUNCON3_VENDOR_PROTOCOL(vend, pr) \
    .match_flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_INT_INFO, \
    .idVendor = (vend), \
    .bInterfaceClass = USB_CLASS_VENDOR_SPEC, \
    .bInterfaceSubClass = 0, \
    .bInterfaceProtocol = (pr)
#define XPAD_GUNCON3_VENDOR(vend) \
    { XPAD_GUNCON3_VENDOR_PROTOCOL((vend), 0) }, \
    { XPAD_GUNCON3_VENDOR_PROTOCOL((vend), 0) }
/*-------------------------------------*/

/* The Xbox One controller uses subclass 71 and protocol 208. */
#define XPAD_XBOXONE_VENDOR_PROTOCOL(vend, pr) \
    .match_flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_INT_INFO, \
    .idVendor = (vend), \
    .bInterfaceClass = USB_CLASS_VENDOR_SPEC, \
    .bInterfaceSubClass = 71, \
    .bInterfaceProtocol = (pr)
#define XPAD_XBOXONE_VENDOR(vend) \
    { XPAD_XBOXONE_VENDOR_PROTOCOL((vend), 208) }

static const struct usb_device_id xpad_table[] = {
	{ USB_INTERFACE_INFO('X', 'B', 0) },	/* X-Box USB-IF not approved class */
	XPAD_XBOX360_VENDOR(0x0079),		/* GPD Win 2 Controller */
	XPAD_XBOX360_VENDOR(0x044f),		/* Thrustmaster X-Box 360 controllers */
	XPAD_XBOX360_VENDOR(0x045e),		/* Microsoft X-Box 360 controllers */
	XPAD_XBOXONE_VENDOR(0x045e),		/* Microsoft X-Box One controllers */
	XPAD_XBOXONE_VENDOR(0x2e24),		/* Hyperkin Duke X-Box One pad */
	XPAD_XBOX360_VENDOR(0x046d),		/* Logitech X-Box 360 style controllers */
	XPAD_XBOX360_VENDOR(0x056e),		/* Elecom JC-U3613M */
	XPAD_XBOX360_VENDOR(0x06a3),		/* Saitek P3600 */
	XPAD_XBOX360_VENDOR(0x0738),		/* Mad Catz X-Box 360 controllers */
	{ USB_DEVICE(0x0738, 0x4540) },		/* Mad Catz Beat Pad */
	XPAD_XBOXONE_VENDOR(0x0738),		/* Mad Catz FightStick TE 2 */
	XPAD_XBOX360_VENDOR(0x07ff),		/* Mad Catz GamePad */
	XPAD_XBOX360_VENDOR(0x0e6f),		/* 0x0e6f X-Box 360 controllers */
	XPAD_XBOXONE_VENDOR(0x0e6f),		/* 0x0e6f X-Box One controllers */
	XPAD_XBOX360_VENDOR(0x0f0d),		/* Hori Controllers */
	XPAD_XBOXONE_VENDOR(0x0f0d),		/* Hori Controllers */
	XPAD_XBOX360_VENDOR(0x11c9),		/* Nacon GC100XF */
	XPAD_XBOX360_VENDOR(0x12ab),		/* X-Box 360 dance pads */
	XPAD_XBOX360_VENDOR(0x1430),		/* RedOctane X-Box 360 controllers */
	XPAD_XBOX360_VENDOR(0x146b),		/* BigBen Interactive Controllers */
	XPAD_XBOX360_VENDOR(0x1532),		/* Razer Sabertooth */
	XPAD_XBOXONE_VENDOR(0x1532),		/* Razer Wildcat */
	XPAD_XBOX360_VENDOR(0x15e4),		/* Numark X-Box 360 controllers */
	XPAD_XBOX360_VENDOR(0x162e),		/* Joytech X-Box 360 controllers */
	XPAD_XBOX360_VENDOR(0x1689),		/* Razer Onza */
	XPAD_XBOX360_VENDOR(0x1bad),		/* Harminix Rock Band Guitar and Drums */
	XPAD_XBOX360_VENDOR(0x24c6),		/* PowerA Controllers */
	XPAD_XBOXONE_VENDOR(0x24c6),		/* PowerA Controllers */
    XPAD_GUNCON3_VENDOR(0x0b9a),
    { }
};

MODULE_DEVICE_TABLE(usb, xpad_table);

struct xboxone_init_packet {
	u16 idVendor;
	u16 idProduct;
	const u8 *data;
	u8 len;
};

#define XBOXONE_INIT_PKT(_vid, _pid, _data)		\
	{						\
		.idVendor	= (_vid),		\
		.idProduct	= (_pid),		\
		.data		= (_data),		\
		.len		= ARRAY_SIZE(_data),	\
	}


/*
 * This packet is required for all Xbox One pads with 2015
 * or later firmware installed (or present from the factory).
 */
static const u8 xboxone_fw2015_init[] = {
	0x05, 0x20, 0x00, 0x01, 0x00
};

/*
 * This packet is required for the Titanfall 2 Xbox One pads
 * (0x0e6f:0x0165) to finish initialization and for Hori pads
 * (0x0f0d:0x0067) to make the analog sticks work.
 */
static const u8 xboxone_hori_init[] = {
	0x01, 0x20, 0x00, 0x09, 0x00, 0x04, 0x20, 0x3a,
	0x00, 0x00, 0x00, 0x80, 0x00
};

/*
 * This packet is required for most (all?) of the PDP pads to start
 * sending input reports. These pads include: (0x0e6f:0x02ab),
 * (0x0e6f:0x02a4), (0x0e6f:0x02a6).
 */
static const u8 xboxone_pdp_init1[] = {
	0x0a, 0x20, 0x00, 0x03, 0x00, 0x01, 0x14
};

/*
 * This packet is required for most (all?) of the PDP pads to start
 * sending input reports. These pads include: (0x0e6f:0x02ab),
 * (0x0e6f:0x02a4), (0x0e6f:0x02a6).
 */
static const u8 xboxone_pdp_init2[] = {
	0x06, 0x20, 0x00, 0x02, 0x01, 0x00
};

static const u8 guncon3_init[]={
    0x01,0x12,0x6F,0x32,0x24,0x60,0x17,0x21
};


/*
 * A specific rumble packet is required for some PowerA pads to start
 * sending input reports. One of those pads is (0x24c6:0x543a).
 */
static const u8 xboxone_rumblebegin_init[] = {
	0x09, 0x00, 0x00, 0x09, 0x00, 0x0F, 0x00, 0x00,
	0x1D, 0x1D, 0xFF, 0x00, 0x00
};

/*
 * A rumble packet with zero FF intensity will immediately
 * terminate the rumbling required to init PowerA pads.
 * This should happen fast enough that the motors don't
 * spin up to enough speed to actually vibrate the gamepad.
 */
static const u8 xboxone_rumbleend_init[] = {
	0x09, 0x00, 0x00, 0x09, 0x00, 0x0F, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00
};

/*
 * This specifies the selection of init packets that a gamepad
 * will be sent on init *and* the order in which they will be
 * sent. The correct sequence number will be added when the
 * packet is going to be sent.
 */
static const struct xboxone_init_packet xboxone_init_packets[] = {
	XBOXONE_INIT_PKT(0x0e6f, 0x0165, xboxone_hori_init),
	XBOXONE_INIT_PKT(0x0f0d, 0x0067, xboxone_hori_init),
	XBOXONE_INIT_PKT(0x0000, 0x0000, xboxone_fw2015_init),
	XBOXONE_INIT_PKT(0x0e6f, 0x0000, xboxone_pdp_init1),
	XBOXONE_INIT_PKT(0x0e6f, 0x0000, xboxone_pdp_init2),
	XBOXONE_INIT_PKT(0x24c6, 0x541a, xboxone_rumblebegin_init),
	XBOXONE_INIT_PKT(0x24c6, 0x542a, xboxone_rumblebegin_init),
	XBOXONE_INIT_PKT(0x24c6, 0x543a, xboxone_rumblebegin_init),
	XBOXONE_INIT_PKT(0x24c6, 0x541a, xboxone_rumbleend_init),
	XBOXONE_INIT_PKT(0x24c6, 0x542a, xboxone_rumbleend_init),
	XBOXONE_INIT_PKT(0x24c6, 0x543a, xboxone_rumbleend_init),
    XBOXONE_INIT_PKT(0x0b9a, 0x0800, guncon3_init),
};

struct xpad_output_packet {
	u8 data[XPAD_PKT_LEN];
	u8 len;
	bool pending;
};

#define XPAD_OUT_CMD_IDX	0
#define XPAD_OUT_FF_IDX		1
#define XPAD_OUT_LED_IDX	(1 + IS_ENABLED(CONFIG_JOYSTICK_XPAD_FF))
#define XPAD_NUM_OUT_PACKETS	(1 + \
				 IS_ENABLED(CONFIG_JOYSTICK_XPAD_FF) + \
				 IS_ENABLED(CONFIG_JOYSTICK_XPAD_LEDS))

struct usb_xpad {
	struct input_dev *dev;		/* input device interface */
	struct input_dev __rcu *x360w_dev;
	struct usb_device *udev;	/* usb device */
	struct usb_interface *intf;	/* usb interface */

	bool pad_present;
	bool input_created;

	struct urb *irq_in;		/* urb for interrupt in report */
	unsigned char *idata;		/* input data */
	dma_addr_t idata_dma;

	struct urb *irq_out;		/* urb for interrupt out report */
	struct usb_anchor irq_out_anchor;
	bool irq_out_active;		/* we must not use an active URB */
	u8 odata_serial;		/* serial number for xbox one protocol */
	unsigned char *odata;		/* output data */
	dma_addr_t odata_dma;
	spinlock_t odata_lock;

	struct xpad_output_packet out_packets[XPAD_NUM_OUT_PACKETS];
	int last_out_packet;
	int init_seq;

#if defined(CONFIG_JOYSTICK_XPAD_LEDS)
	struct xpad_led *led;
#endif

	char phys[64];			/* physical device path */

	int mapping;			/* map d-pad to buttons or to axes */
	int xtype;			/* type of xbox device */
	int pad_nr;			/* the order x360 pads were attached */
	const char *name;		/* name of the device */
	struct work_struct work;	/* init/remove device from callback */
    char guncon3_key_sended;
};

static int xpad_init_input(struct usb_xpad *xpad);
static void xpad_deinit_input(struct usb_xpad *xpad);
static void xpadone_ack_mode_report(struct usb_xpad *xpad, u8 seq_num);

/*
 *	xpad_process_packet
 *
 *	Completes a request by converting the data into events for the
 *	input subsystem.
 *
 *	The used report descriptor was taken from ITO Takayukis website:
 *	 http://euc.jp/periphs/xbox-controller.ja.html
 */
static void xpad_process_packet(struct usb_xpad *xpad, u16 cmd, unsigned char *data)
{
	struct input_dev *dev = xpad->dev;

	if (!(xpad->mapping & MAP_STICKS_TO_NULL)) {
		/* left stick */
		input_report_abs(dev, ABS_X,
				 (__s16) le16_to_cpup((__le16 *)(data + 12)));
		input_report_abs(dev, ABS_Y,
				 ~(__s16) le16_to_cpup((__le16 *)(data + 14)));

		/* right stick */
		input_report_abs(dev, ABS_RX,
				 (__s16) le16_to_cpup((__le16 *)(data + 16)));
		input_report_abs(dev, ABS_RY,
				 ~(__s16) le16_to_cpup((__le16 *)(data + 18)));
	}

	/* triggers left/right */
	if (xpad->mapping & MAP_TRIGGERS_TO_BUTTONS) {
		input_report_key(dev, BTN_TL2, data[10]);
		input_report_key(dev, BTN_TR2, data[11]);
	} else {
		input_report_abs(dev, ABS_Z, data[10]);
		input_report_abs(dev, ABS_RZ, data[11]);
	}

	/* digital pad */
	if (xpad->mapping & MAP_DPAD_TO_BUTTONS) {
		/* dpad as buttons (left, right, up, down) */
		input_report_key(dev, BTN_TRIGGER_HAPPY1, data[2] & 0x04);
		input_report_key(dev, BTN_TRIGGER_HAPPY2, data[2] & 0x08);
		input_report_key(dev, BTN_TRIGGER_HAPPY3, data[2] & 0x01);
		input_report_key(dev, BTN_TRIGGER_HAPPY4, data[2] & 0x02);
	} else {
		input_report_abs(dev, ABS_HAT0X,
				 !!(data[2] & 0x08) - !!(data[2] & 0x04));
		input_report_abs(dev, ABS_HAT0Y,
				 !!(data[2] & 0x02) - !!(data[2] & 0x01));
	}

	/* start/back buttons and stick press left/right */
	input_report_key(dev, BTN_START,  data[2] & 0x10);
	input_report_key(dev, BTN_SELECT, data[2] & 0x20);
	input_report_key(dev, BTN_THUMBL, data[2] & 0x40);
	input_report_key(dev, BTN_THUMBR, data[2] & 0x80);

	/* "analog" buttons A, B, X, Y */
	input_report_key(dev, BTN_A, data[4]);
	input_report_key(dev, BTN_B, data[5]);
	input_report_key(dev, BTN_X, data[6]);
	input_report_key(dev, BTN_Y, data[7]);

	/* "analog" buttons black, white */
	input_report_key(dev, BTN_C, data[8]);
	input_report_key(dev, BTN_Z, data[9]);

	input_sync(dev);
}

/*
 *	xpad360_process_packet
 *
 *	Completes a request by converting the data into events for the
 *	input subsystem. It is version for xbox 360 controller
 *
 *	The used report descriptor was taken from:
 *		http://www.free60.org/wiki/Gamepad
 */

static void xpad360_process_packet(struct usb_xpad *xpad, struct input_dev *dev,
				   u16 cmd, unsigned char *data)
{
	/* valid pad data */
	if (data[0] != 0x00)
		return;

	/* digital pad */
	if (xpad->mapping & MAP_DPAD_TO_BUTTONS) {
		/* dpad as buttons (left, right, up, down) */
		input_report_key(dev, BTN_TRIGGER_HAPPY1, data[2] & 0x04);
		input_report_key(dev, BTN_TRIGGER_HAPPY2, data[2] & 0x08);
		input_report_key(dev, BTN_TRIGGER_HAPPY3, data[2] & 0x01);
		input_report_key(dev, BTN_TRIGGER_HAPPY4, data[2] & 0x02);
	}

	/*
	 * This should be a simple else block. However historically
	 * xbox360w has mapped DPAD to buttons while xbox360 did not. This
	 * made no sense, but now we can not just switch back and have to
	 * support both behaviors.
	 */
	if (!(xpad->mapping & MAP_DPAD_TO_BUTTONS) ||
	    xpad->xtype == XTYPE_XBOX360W) {
		input_report_abs(dev, ABS_HAT0X,
				 !!(data[2] & 0x08) - !!(data[2] & 0x04));
		input_report_abs(dev, ABS_HAT0Y,
				 !!(data[2] & 0x02) - !!(data[2] & 0x01));
	}

	/* start/back buttons */
	input_report_key(dev, BTN_START,  data[2] & 0x10);
	input_report_key(dev, BTN_SELECT, data[2] & 0x20);

	/* stick press left/right */
	input_report_key(dev, BTN_THUMBL, data[2] & 0x40);
	input_report_key(dev, BTN_THUMBR, data[2] & 0x80);

	/* buttons A,B,X,Y,TL,TR and MODE */
	input_report_key(dev, BTN_A,	data[3] & 0x10);
	input_report_key(dev, BTN_B,	data[3] & 0x20);
	input_report_key(dev, BTN_X,	data[3] & 0x40);
	input_report_key(dev, BTN_Y,	data[3] & 0x80);
	input_report_key(dev, BTN_TL,	data[3] & 0x01);
	input_report_key(dev, BTN_TR,	data[3] & 0x02);
	input_report_key(dev, BTN_MODE,	data[3] & 0x04);

	if (!(xpad->mapping & MAP_STICKS_TO_NULL)) {
		/* left stick */
		input_report_abs(dev, ABS_X,
				 (__s16) le16_to_cpup((__le16 *)(data + 6)));
		input_report_abs(dev, ABS_Y,
				 ~(__s16) le16_to_cpup((__le16 *)(data + 8)));

		/* right stick */
		input_report_abs(dev, ABS_RX,
				 (__s16) le16_to_cpup((__le16 *)(data + 10)));
		input_report_abs(dev, ABS_RY,
				 ~(__s16) le16_to_cpup((__le16 *)(data + 12)));
	}

	/* triggers left/right */
	if (xpad->mapping & MAP_TRIGGERS_TO_BUTTONS) {
		input_report_key(dev, BTN_TL2, data[4]);
		input_report_key(dev, BTN_TR2, data[5]);
	} else {
		input_report_abs(dev, ABS_Z, data[4]);
		input_report_abs(dev, ABS_RZ, data[5]);
	}

	input_sync(dev);
}

// tabla de decodificación de guncon3
static const unsigned char KEY_TABLE[320] = {
    0x75, 0xC3, 0x10, 0x31, 0xB5, 0xD3, 0x69, 0x84, 0x89, 0xBA, 0xD6, 0x89, 0xBD, 0x70, 0x19, 0x8E, 0x58, 0xA8,
    0x3D, 0x9B, 0x5D, 0xF0, 0x49, 0xE8, 0xAD, 0x9D, 0x7A, 0xD, 0x7E, 0x24, 0xDA, 0xFC, 0xD, 0x14, 0xC5, 0x23,
    0x91, 0x11, 0xF5, 0xC0, 0x4B, 0xCD, 0x44, 0x1C, 0xC5, 0x21, 0xDF, 0x61, 0x54, 0xED, 0xA2, 0x81, 0xB7, 0xE5,
    0x74, 0x94, 0xB0, 0x47, 0xEE, 0xF1, 0xA5, 0xBB, 0x21, 0xC8, 0x91, 0xFD, 0x4C, 0x8B, 0x20, 0xC1, 0x7C, 9, 0x58,
    0x14, 0xF6, 0, 0x52, 0x55, 0xBF, 0x41, 0x75, 0xC0, 0x13, 0x30, 0xB5, 0xD0, 0x69, 0x85, 0x89, 0xBB, 0xD6, 0x88,
    0xBC, 0x73, 0x18, 0x8D, 0x58, 0xAB, 0x3D, 0x98, 0x5C, 0xF2, 0x48, 0xE9, 0xAC, 0x9F, 0x7A, 0xC, 0x7C, 0x25, 0xD8,
    0xFF, 0xDC, 0x7D, 8, 0xDB, 0xBC, 0x18, 0x8C, 0x1D, 0xD6, 0x3C, 0x35, 0xE1, 0x2C, 0x14, 0x8E, 0x64, 0x83, 0x39,
    0xB0, 0xE4, 0x4E, 0xF7, 0x51, 0x7B, 0xA8, 0x13, 0xAC, 0xE9, 0x43, 0xC0, 8, 0x25, 0xE, 0x15, 0xC4, 0x20, 0x93,
    0x13, 0xF5, 0xC3, 0x48, 0xCC, 0x47, 0x1C, 0xC5, 0x20, 0xDE, 0x60, 0x55, 0xEE, 0xA0, 0x40, 0xB4, 0xE7, 0x74,
    0x95, 0xB0, 0x46, 0xEC, 0xF0, 0xA5, 0xB8, 0x23, 0xC8, 4, 6, 0xFC, 0x28, 0xCB, 0xF8, 0x17, 0x2C, 0x25, 0x1C,
    0xCB, 0x18, 0xE3, 0x6C, 0x80, 0x85, 0xDD, 0x7E, 9, 0xD9, 0xBC, 0x19, 0x8F, 0x1D, 0xD4, 0x3D, 0x37, 0xE1, 0x2F,
    0x15, 0x8D, 0x64, 6, 4, 0xFD, 0x29, 0xCF, 0xFA, 0x14, 0x2E, 0x25, 0x1F, 0xC9, 0x18, 0xE3, 0x6D, 0x81, 0x84,
    0x80, 0x3B, 0xB1, 0xE5, 0x4D, 0xF7, 0x51, 0x78, 0xA9, 0x13, 0xAD, 0xE9, 0x80, 0xC1, 0xB, 0x25, 0x93, 0xFC,
    0x4D, 0x89, 0x23, 0xC2, 0x7C, 0xB, 0x59, 0x15, 0xF6, 1, 0x50, 0x55, 0xBF, 0x81, 0x75, 0xC3, 0x10, 0x31, 0xB5,
    0xD3, 0x69, 0x84, 0x89, 0xBA, 0xD6, 0x89, 0xBD, 0x70, 0x19, 0x8E, 0x58, 0xA8, 0x3D, 0x9B, 0x5D, 0xF0, 0x49,
    0xE8, 0xAD, 0x9D, 0x7A, 0xD, 0x7E, 0x24, 0xDA, 0xFC, 0xD, 0x14, 0xC5, 0x23, 0x91, 0x11, 0xF5, 0xC0, 0x4B, 0xCD,
    0x44, 0x1C, 0xC5, 0x21, 0xDF, 0x61, 0x54, 0xED, 0xA2, 0x81, 0xB7, 0xE5, 0x74, 0x94, 0xB0, 0x47, 0xEE, 0xF1,
    0xA5, 0xBB, 0x21, 0xC8
};
//función de decodificación de trama guncon3
static int guncon3_decode(unsigned char *data, const unsigned char *key) {
    int x, y, key_index;
    int bkey, keyr, byte;
    int a_sum,b_sum;
    int key_offset;
    //unsigned char data[15];

        b_sum = data[13] ^ data[12];
        b_sum = b_sum + data[11] + data[10] - data[9] - data[8];
        b_sum = b_sum ^ data[7];
        b_sum = b_sum & 0xFF;
        a_sum = data[6] ^ b_sum;
        a_sum = a_sum - data[5] - data[4];
        a_sum = a_sum ^ data[3];
        a_sum = a_sum + data[2] + data[1] - data[0];
        a_sum = a_sum & 0xFF;

        if (a_sum != key[7]) {
        return -1;
    }

    //key_offset = (((((key[1] ^ key[2]) - key[3] - key[4]) ^ key[5]) + key[6] - key[7]) ^ data[14]) + (unsigned char)0x26;
        key_offset = key[1] ^ key[2];
          key_offset = key_offset - key[3] - key[4];
          key_offset = key_offset ^ key[5];
          key_offset = key_offset + key[6] - key[7];
          key_offset = key_offset ^ data[14];
          key_offset = key_offset + 0x26;
          key_offset = key_offset & 0xFF;

    key_index = 4;
    //byte E is part of the key offset
    // byte D is ignored, possibly a padding byte - make the checksum workout
    for (x = 12; x >= 0; x--) {
        byte = data[x];
        for (y = 4; y > 1; y--) { // loop 3 times
            key_offset--;

            bkey = KEY_TABLE[key_offset + 0x41];
            keyr = key[key_index];
            if (--key_index == 0)
                key_index = 7;

            if ((bkey & 3) == 0)
                byte =(byte - bkey) - keyr;
            else if ((bkey & 3) == 1)
                byte = ((byte + bkey) + keyr);
            else
                byte = ((byte ^ bkey) ^ keyr);
        }
        data[x] = byte;
    }

    return 0;
}

static void guncon3_process_packet(struct usb_xpad *xpad, u16 cmd, unsigned char *data)
{
    //decodificar recepcion
    struct input_dev *dev = xpad->dev;
    guncon3_decode(data,guncon3_init);
    //aplicar a input

    /* aim */
    input_report_abs(dev, ABS_X,
             (__s16) le16_to_cpup((__le16 *)(data + 3)));
    input_report_abs(dev, ABS_Y,
             ~(__s16) le16_to_cpup((__le16 *)(data + 5)));

    // Z axis (data[8] << 8) + data[7]

    /* joystick a/b */
    input_report_abs(dev, ABS_RX, data[11]);
    input_report_abs(dev, ABS_RY, data[12]);
    input_report_abs(dev, ABS_HAT0X, data[9]);
    input_report_abs(dev, ABS_HAT0Y, data[10]);

    /* buttons */
    input_report_key(dev, BTN_TRIGGER, (data[1] & 0x20));
    input_report_key(dev, BTN_0, (data[0] & 0x04));    // A
    input_report_key(dev, BTN_1, (data[0] & 0x02));
    input_report_key(dev, BTN_2, (data[1] & 0x04));    // B
    input_report_key(dev, BTN_3, (data[1] & 0x02));
    input_report_key(dev, BTN_4, (data[1] & 0x80));    // C
    input_report_key(dev, BTN_5, (data[0] & 0x08));
    input_report_key(dev, BTN_6, (data[2] & 0x80));    // Joystick buttons
    input_report_key(dev, BTN_7, (data[2] & 0x40));

    input_sync(dev);
}

static void xpad_presence_work(struct work_struct *work)
{
	struct usb_xpad *xpad = container_of(work, struct usb_xpad, work);
	int error;

	if (xpad->pad_present) {
		error = xpad_init_input(xpad);
		if (error) {
			/* complain only, not much else we can do here */
			dev_err(&xpad->dev->dev,
				"unable to init device: %d\n", error);
		} else {
			rcu_assign_pointer(xpad->x360w_dev, xpad->dev);
		}
	} else {
        //printk(KERN_INFO "No sé que coño hace aqui\n");
		RCU_INIT_POINTER(xpad->x360w_dev, NULL);
		synchronize_rcu();
		/*
		 * Now that we are sure xpad360w_process_packet is not
		 * using input device we can get rid of it.
		 */
		xpad_deinit_input(xpad);
	}
}

/*
 * xpad360w_process_packet
 *
 * Completes a request by converting the data into events for the
 * input subsystem. It is version for xbox 360 wireless controller.
 *
 * Byte.Bit
 * 00.1 - Status change: The controller or headset has connected/disconnected
 *                       Bits 01.7 and 01.6 are valid
 * 01.7 - Controller present
 * 01.6 - Headset present
 * 01.1 - Pad state (Bytes 4+) valid
 *
 */
static void xpad360w_process_packet(struct usb_xpad *xpad, u16 cmd, unsigned char *data)
{
	struct input_dev *dev;
	bool present;

	/* Presence change */
	if (data[0] & 0x08) {
		present = (data[1] & 0x80) != 0;

		if (xpad->pad_present != present) {
			xpad->pad_present = present;
			schedule_work(&xpad->work);
		}
	}

	/* Valid pad data */
	if (data[1] != 0x1)
		return;

	rcu_read_lock();
	dev = rcu_dereference(xpad->x360w_dev);
	if (dev)
		xpad360_process_packet(xpad, dev, cmd, &data[4]);
	rcu_read_unlock();
}

/*
 *	xpadone_process_packet
 *
 *	Completes a request by converting the data into events for the
 *	input subsystem. This version is for the Xbox One controller.
 *
 *	The report format was gleaned from
 *	https://github.com/kylelemons/xbox/blob/master/xbox.go
 */
static void xpadone_process_packet(struct usb_xpad *xpad, u16 cmd, unsigned char *data)
{
	struct input_dev *dev = xpad->dev;

	/* the xbox button has its own special report */
	if (data[0] == 0X07) {
		/*
		 * The Xbox One S controller requires these reports to be
		 * acked otherwise it continues sending them forever and
		 * won't report further mode button events.
		 */
		if (data[1] == 0x30)
			xpadone_ack_mode_report(xpad, data[2]);

		input_report_key(dev, BTN_MODE, data[4] & 0x01);
		input_sync(dev);
		return;
	}
	/* check invalid packet */
	else if (data[0] != 0X20)
		return;

	/* menu/view buttons */
	input_report_key(dev, BTN_START,  data[4] & 0x04);
	input_report_key(dev, BTN_SELECT, data[4] & 0x08);

	/* buttons A,B,X,Y */
	input_report_key(dev, BTN_A,	data[4] & 0x10);
	input_report_key(dev, BTN_B,	data[4] & 0x20);
	input_report_key(dev, BTN_X,	data[4] & 0x40);
	input_report_key(dev, BTN_Y,	data[4] & 0x80);

	/* digital pad */
	if (xpad->mapping & MAP_DPAD_TO_BUTTONS) {
		/* dpad as buttons (left, right, up, down) */
		input_report_key(dev, BTN_TRIGGER_HAPPY1, data[5] & 0x04);
		input_report_key(dev, BTN_TRIGGER_HAPPY2, data[5] & 0x08);
		input_report_key(dev, BTN_TRIGGER_HAPPY3, data[5] & 0x01);
		input_report_key(dev, BTN_TRIGGER_HAPPY4, data[5] & 0x02);
	} else {
		input_report_abs(dev, ABS_HAT0X,
				 !!(data[5] & 0x08) - !!(data[5] & 0x04));
		input_report_abs(dev, ABS_HAT0Y,
				 !!(data[5] & 0x02) - !!(data[5] & 0x01));
	}

	/* TL/TR */
	input_report_key(dev, BTN_TL,	data[5] & 0x10);
	input_report_key(dev, BTN_TR,	data[5] & 0x20);

	/* stick press left/right */
	input_report_key(dev, BTN_THUMBL, data[5] & 0x40);
	input_report_key(dev, BTN_THUMBR, data[5] & 0x80);

	if (!(xpad->mapping & MAP_STICKS_TO_NULL)) {
		/* left stick */
		input_report_abs(dev, ABS_X,
				 (__s16) le16_to_cpup((__le16 *)(data + 10)));
		input_report_abs(dev, ABS_Y,
				 ~(__s16) le16_to_cpup((__le16 *)(data + 12)));

		/* right stick */
		input_report_abs(dev, ABS_RX,
				 (__s16) le16_to_cpup((__le16 *)(data + 14)));
		input_report_abs(dev, ABS_RY,
				 ~(__s16) le16_to_cpup((__le16 *)(data + 16)));
	}

	/* triggers left/right */
	if (xpad->mapping & MAP_TRIGGERS_TO_BUTTONS) {
		input_report_key(dev, BTN_TL2,
				 (__u16) le16_to_cpup((__le16 *)(data + 6)));
		input_report_key(dev, BTN_TR2,
				 (__u16) le16_to_cpup((__le16 *)(data + 8)));
	} else {
		input_report_abs(dev, ABS_Z,
				 (__u16) le16_to_cpup((__le16 *)(data + 6)));
		input_report_abs(dev, ABS_RZ,
				 (__u16) le16_to_cpup((__le16 *)(data + 8)));
	}

	input_sync(dev);
}

static void xpad_irq_in(struct urb *urb)
{
	struct usb_xpad *xpad = urb->context;
	struct device *dev = &xpad->intf->dev;
	int retval, status;
    //printk(KERN_INFO "xpad_irq_in \n");
	status = urb->status;
//printk(KERN_INFO "irq status %d\n",status);
	switch (status) {
	case 0:
		/* success */
        //printk(KERN_INFO "irq in success\n");
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dev_dbg(dev, "%s - urb shutting down with status: %d\n",
			__func__, status);
		return;
	default:
		dev_dbg(dev, "%s - nonzero urb status received: %d\n",
			__func__, status);
		goto exit;
	}

#if defined(DEBUG_VERBOSE)
	/* If you set rowsize to larger than 32 it defaults to 16?
	 * Otherwise I would set it to XPAD_PKT_LEN                  V
	 */
	print_hex_dump(KERN_DEBUG, "xpad-dbg: ", DUMP_PREFIX_OFFSET, 32, 1, xpad->idata, XPAD_PKT_LEN, 0);
#endif
   // print_hex_dump(KERN_DEBUG, "xpad-dbg: ", DUMP_PREFIX_OFFSET, 32, 1, xpad->idata, XPAD_PKT_LEN_GUNCON3_IN, 0);
	switch (xpad->xtype) {
	case XTYPE_XBOX360:
		xpad360_process_packet(xpad, xpad->dev, 0, xpad->idata);
		break;
	case XTYPE_XBOX360W:
		xpad360w_process_packet(xpad, 0, xpad->idata);
		break;
	case XTYPE_XBOXONE:
		xpadone_process_packet(xpad, 0, xpad->idata);
		break;
    case XTYPE_GUNCON3:
        //printk(KERN_INFO "Process guncon3\n");
        guncon3_process_packet(xpad, 0, xpad->idata);
        break;
	default:
		xpad_process_packet(xpad, 0, xpad->idata);
	}

exit:
	retval = usb_submit_urb(urb, GFP_ATOMIC);
	if (retval)
		dev_err(dev, "%s - usb_submit_urb failed with result %d\n",
			__func__, retval);
}

/* Callers must hold xpad->odata_lock spinlock */
static bool xpad_prepare_next_init_packet(struct usb_xpad *xpad)
{
	const struct xboxone_init_packet *init_packet;
    //printk(KERN_INFO "INIT key\n");

    if( xpad->xtype == XTYPE_GUNCON3)
    {
        xpad->init_seq=11;
        init_packet = &xboxone_init_packets[xpad->init_seq++];
        if(init_packet->idVendor == 0x0b9a)
        {
            //printk(KERN_INFO "preparando siguiente paquete de inicio\n");
        }
        memcpy(xpad->odata, init_packet->data, init_packet->len);
        xpad->irq_out->transfer_buffer_length = init_packet->len;
        return true;

    }
	if (xpad->xtype != XTYPE_XBOXONE)
		return false;

	/* Perform initialization sequence for Xbox One pads that require it */
	while (xpad->init_seq < ARRAY_SIZE(xboxone_init_packets)) {
		init_packet = &xboxone_init_packets[xpad->init_seq++];

		if (init_packet->idVendor != 0 &&
		    init_packet->idVendor != xpad->dev->id.vendor)
			continue;

		if (init_packet->idProduct != 0 &&
		    init_packet->idProduct != xpad->dev->id.product)
			continue;

		/* This packet applies to our device, so prepare to send it */
        //printk( KERN_INFO "Preparando para mandar init: tamanyo %d\n",init_packet->len);
		memcpy(xpad->odata, init_packet->data, init_packet->len);
		xpad->irq_out->transfer_buffer_length = init_packet->len;

		/* Update packet with current sequence number */
        xpad->odata[2] = xpad->odata_serial++;

		return true;
	}

	return false;
}

/* Callers must hold xpad->odata_lock spinlock */
static bool xpad_prepare_next_out_packet(struct usb_xpad *xpad)
{
	struct xpad_output_packet *pkt, *packet = NULL;
	int i;

	/* We may have init packets to send before we can send user commands */
	if (xpad_prepare_next_init_packet(xpad))
    {
        //printk(KERN_INFO "prepare init devuleve true\n");
		return true;
    }
    if(xpad->xtype != XTYPE_GUNCON3){


	for (i = 0; i < XPAD_NUM_OUT_PACKETS; i++) {
		if (++xpad->last_out_packet >= XPAD_NUM_OUT_PACKETS)
			xpad->last_out_packet = 0;

		pkt = &xpad->out_packets[xpad->last_out_packet];
		if (pkt->pending) {
			dev_dbg(&xpad->intf->dev,
				"%s - found pending output packet %d\n",
				__func__, xpad->last_out_packet);
			packet = pkt;
			break;
		}
	}

	if (packet) {
		memcpy(xpad->odata, packet->data, packet->len);
		xpad->irq_out->transfer_buffer_length = packet->len;
		packet->pending = false;
		return true;
	}
    }
    else {
        //esto solo para guncon3
        //printk(KERN_INFO "Copiando out data \n");
        if(xpad->guncon3_key_sended==0)
        {
        xpad->last_out_packet = 0;
        memcpy(xpad->odata, guncon3_init, 8);
        xpad->irq_out->transfer_buffer_length = 8;
        packet->pending = false;
        xpad->guncon3_key_sended=1;
        return true;
        }
        else {
            return false;
        }
    }
	return false;
}

/* Callers must hold xpad->odata_lock spinlock */
static int xpad_try_sending_next_out_packet(struct usb_xpad *xpad)
{
	int error;

	if (!xpad->irq_out_active && xpad_prepare_next_out_packet(xpad)) {
		usb_anchor_urb(xpad->irq_out, &xpad->irq_out_anchor);
        //printk(KERN_INFO "Prepare submit out\n");
		error = usb_submit_urb(xpad->irq_out, GFP_ATOMIC);
		if (error) {
            //printk(KERN_INFO "error al submit out\n");
			dev_err(&xpad->intf->dev,
				"%s - usb_submit_urb failed with result %d\n",
				__func__, error);
			usb_unanchor_urb(xpad->irq_out);
			return -EIO;
		}

		xpad->irq_out_active = true;
	}

	return 0;
}

static void xpad_irq_out(struct urb *urb)
{
	struct usb_xpad *xpad = urb->context;
	struct device *dev = &xpad->intf->dev;
	int status = urb->status;
	int error;
	unsigned long flags;
    //printk(KERN_INFO "xpad_irq_out\n");
	spin_lock_irqsave(&xpad->odata_lock, flags);

	switch (status) {
	case 0:
		/* success */
		xpad->irq_out_active = xpad_prepare_next_out_packet(xpad);
		break;

	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dev_dbg(dev, "%s - urb shutting down with status: %d\n",
			__func__, status);
		xpad->irq_out_active = false;
		break;

	default:
		dev_dbg(dev, "%s - nonzero urb status received: %d\n",
			__func__, status);
		break;
	}

	if (xpad->irq_out_active) {
		usb_anchor_urb(urb, &xpad->irq_out_anchor);
		error = usb_submit_urb(urb, GFP_ATOMIC);
		if (error) {
			dev_err(dev,
				"%s - usb_submit_urb failed with result %d\n",
				__func__, error);
			usb_unanchor_urb(urb);
			xpad->irq_out_active = false;
		}
	}

	spin_unlock_irqrestore(&xpad->odata_lock, flags);
}

static int xpad_init_output(struct usb_interface *intf, struct usb_xpad *xpad,
			struct usb_endpoint_descriptor *ep_irq_out)
{
	int error;

	if (xpad->xtype == XTYPE_UNKNOWN)
		return 0;

	init_usb_anchor(&xpad->irq_out_anchor);
    if(xpad->xtype==XTYPE_GUNCON3)
    {
        xpad->odata = usb_alloc_coherent(xpad->udev, XPAD_PKT_LEN_GUNCON3,
                         GFP_KERNEL, &xpad->odata_dma);
    }
    else
    {


	xpad->odata = usb_alloc_coherent(xpad->udev, XPAD_PKT_LEN,
					 GFP_KERNEL, &xpad->odata_dma);
    }
    if (!xpad->odata)
		return -ENOMEM;

	spin_lock_init(&xpad->odata_lock);

	xpad->irq_out = usb_alloc_urb(0, GFP_KERNEL);
	if (!xpad->irq_out) {
		error = -ENOMEM;
		goto err_free_coherent;
	}

    if(xpad->xtype== XTYPE_GUNCON3)
    {
        usb_fill_int_urb(xpad->irq_out, xpad->udev,
                 usb_sndintpipe(xpad->udev, ep_irq_out->bEndpointAddress),
                 xpad->odata, XPAD_PKT_LEN_GUNCON3,
                 xpad_irq_out, xpad, ep_irq_out->bInterval);
    }
    else
    {
     usb_fill_int_urb(xpad->irq_out, xpad->udev,
			 usb_sndintpipe(xpad->udev, ep_irq_out->bEndpointAddress),
			 xpad->odata, XPAD_PKT_LEN,
			 xpad_irq_out, xpad, ep_irq_out->bInterval);
    }
    xpad->irq_out->transfer_dma = xpad->odata_dma;
	xpad->irq_out->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	return 0;

err_free_coherent:
	usb_free_coherent(xpad->udev, XPAD_PKT_LEN, xpad->odata, xpad->odata_dma);
	return error;
}

static void xpad_stop_output(struct usb_xpad *xpad)
{
	if (xpad->xtype != XTYPE_UNKNOWN) {
		if (!usb_wait_anchor_empty_timeout(&xpad->irq_out_anchor,
						   5000)) {
			dev_warn(&xpad->intf->dev,
				 "timed out waiting for output URB to complete, killing\n");
			usb_kill_anchored_urbs(&xpad->irq_out_anchor);
		}
	}
}

static void xpad_deinit_output(struct usb_xpad *xpad)
{
	if (xpad->xtype != XTYPE_UNKNOWN) {
		usb_free_urb(xpad->irq_out);
		usb_free_coherent(xpad->udev, XPAD_PKT_LEN,
				xpad->odata, xpad->odata_dma);
	}
}

static int xpad_inquiry_pad_presence(struct usb_xpad *xpad)
{
	struct xpad_output_packet *packet =
			&xpad->out_packets[XPAD_OUT_CMD_IDX];
	unsigned long flags;
	int retval;

	spin_lock_irqsave(&xpad->odata_lock, flags);

	packet->data[0] = 0x08;
	packet->data[1] = 0x00;
	packet->data[2] = 0x0F;
	packet->data[3] = 0xC0;
	packet->data[4] = 0x00;
	packet->data[5] = 0x00;
	packet->data[6] = 0x00;
	packet->data[7] = 0x00;
	packet->data[8] = 0x00;
	packet->data[9] = 0x00;
	packet->data[10] = 0x00;
	packet->data[11] = 0x00;
	packet->len = 12;
	packet->pending = true;

	/* Reset the sequence so we send out presence first */
	xpad->last_out_packet = -1;
	retval = xpad_try_sending_next_out_packet(xpad);

	spin_unlock_irqrestore(&xpad->odata_lock, flags);

	return retval;
}

static int xpad_start_xbox_one(struct usb_xpad *xpad)
{
	unsigned long flags;
	int retval;

	spin_lock_irqsave(&xpad->odata_lock, flags);

	/*
	 * Begin the init sequence by attempting to send a packet.
	 * We will cycle through the init packet sequence before
	 * sending any packets from the output ring.
	 */
	xpad->init_seq = 0;
	retval = xpad_try_sending_next_out_packet(xpad);

	spin_unlock_irqrestore(&xpad->odata_lock, flags);

	return retval;
}

static void xpadone_ack_mode_report(struct usb_xpad *xpad, u8 seq_num)
{
	unsigned long flags;
	struct xpad_output_packet *packet =
			&xpad->out_packets[XPAD_OUT_CMD_IDX];
	static const u8 mode_report_ack[] = {
		0x01, 0x20, 0x00, 0x09, 0x00, 0x07, 0x20, 0x02,
		0x00, 0x00, 0x00, 0x00, 0x00
	};

	spin_lock_irqsave(&xpad->odata_lock, flags);

	packet->len = sizeof(mode_report_ack);
	memcpy(packet->data, mode_report_ack, packet->len);
	packet->data[2] = seq_num;
	packet->pending = true;

	/* Reset the sequence so we send out the ack now */
	xpad->last_out_packet = -1;
	xpad_try_sending_next_out_packet(xpad);

	spin_unlock_irqrestore(&xpad->odata_lock, flags);
}

#ifdef CONFIG_JOYSTICK_XPAD_FF
static int xpad_play_effect(struct input_dev *dev, void *data, struct ff_effect *effect)
{
	struct usb_xpad *xpad = input_get_drvdata(dev);
	struct xpad_output_packet *packet = &xpad->out_packets[XPAD_OUT_FF_IDX];
	__u16 strong;
	__u16 weak;
	int retval;
	unsigned long flags;

	if (effect->type != FF_RUMBLE)
		return 0;

	strong = effect->u.rumble.strong_magnitude;
	weak = effect->u.rumble.weak_magnitude;

	spin_lock_irqsave(&xpad->odata_lock, flags);

	switch (xpad->xtype) {
	case XTYPE_XBOX:
		packet->data[0] = 0x00;
		packet->data[1] = 0x06;
		packet->data[2] = 0x00;
		packet->data[3] = strong / 256;	/* left actuator */
		packet->data[4] = 0x00;
		packet->data[5] = weak / 256;	/* right actuator */
		packet->len = 6;
		packet->pending = true;
		break;

	case XTYPE_XBOX360:
		packet->data[0] = 0x00;
		packet->data[1] = 0x08;
		packet->data[2] = 0x00;
		packet->data[3] = strong / 256;  /* left actuator? */
		packet->data[4] = weak / 256;	/* right actuator? */
		packet->data[5] = 0x00;
		packet->data[6] = 0x00;
		packet->data[7] = 0x00;
		packet->len = 8;
		packet->pending = true;
		break;

	case XTYPE_XBOX360W:
		packet->data[0] = 0x00;
		packet->data[1] = 0x01;
		packet->data[2] = 0x0F;
		packet->data[3] = 0xC0;
		packet->data[4] = 0x00;
		packet->data[5] = strong / 256;
		packet->data[6] = weak / 256;
		packet->data[7] = 0x00;
		packet->data[8] = 0x00;
		packet->data[9] = 0x00;
		packet->data[10] = 0x00;
		packet->data[11] = 0x00;
		packet->len = 12;
		packet->pending = true;
		break;

	case XTYPE_XBOXONE:
		packet->data[0] = 0x09; /* activate rumble */
		packet->data[1] = 0x00;
		packet->data[2] = xpad->odata_serial++;
		packet->data[3] = 0x09;
		packet->data[4] = 0x00;
		packet->data[5] = 0x0F;
		packet->data[6] = 0x00;
		packet->data[7] = 0x00;
		packet->data[8] = strong / 512;	/* left actuator */
		packet->data[9] = weak / 512;	/* right actuator */
		packet->data[10] = 0xFF; /* on period */
		packet->data[11] = 0x00; /* off period */
		packet->data[12] = 0xFF; /* repeat count */
		packet->len = 13;
		packet->pending = true;
		break;

	default:
		dev_dbg(&xpad->dev->dev,
			"%s - rumble command sent to unsupported xpad type: %d\n",
			__func__, xpad->xtype);
		retval = -EINVAL;
		goto out;
	}

	retval = xpad_try_sending_next_out_packet(xpad);

out:
	spin_unlock_irqrestore(&xpad->odata_lock, flags);
	return retval;
}

static int xpad_init_ff(struct usb_xpad *xpad)
{
	if (xpad->xtype == XTYPE_UNKNOWN)
		return 0;

	input_set_capability(xpad->dev, EV_FF, FF_RUMBLE);

	return input_ff_create_memless(xpad->dev, NULL, xpad_play_effect);
}

#else
static int xpad_init_ff(struct usb_xpad *xpad) { return 0; }
#endif

#if defined(CONFIG_JOYSTICK_XPAD_LEDS)
#include <linux/leds.h>
#include <linux/idr.h>

static DEFINE_IDA(xpad_pad_seq);

struct xpad_led {
	char name[16];
	struct led_classdev led_cdev;
	struct usb_xpad *xpad;
};

/**
 * set the LEDs on Xbox360 / Wireless Controllers
 * @param command
 *  0: off
 *  1: all blink, then previous setting
 *  2: 1/top-left blink, then on
 *  3: 2/top-right blink, then on
 *  4: 3/bottom-left blink, then on
 *  5: 4/bottom-right blink, then on
 *  6: 1/top-left on
 *  7: 2/top-right on
 *  8: 3/bottom-left on
 *  9: 4/bottom-right on
 * 10: rotate
 * 11: blink, based on previous setting
 * 12: slow blink, based on previous setting
 * 13: rotate with two lights
 * 14: persistent slow all blink
 * 15: blink once, then previous setting
 */
static void xpad_send_led_command(struct usb_xpad *xpad, int command)
{
	struct xpad_output_packet *packet =
			&xpad->out_packets[XPAD_OUT_LED_IDX];
	unsigned long flags;

	command %= 16;

	spin_lock_irqsave(&xpad->odata_lock, flags);

	switch (xpad->xtype) {
	case XTYPE_XBOX360:
		packet->data[0] = 0x01;
		packet->data[1] = 0x03;
		packet->data[2] = command;
		packet->len = 3;
		packet->pending = true;
		break;

	case XTYPE_XBOX360W:
		packet->data[0] = 0x00;
		packet->data[1] = 0x00;
		packet->data[2] = 0x08;
		packet->data[3] = 0x40 + command;
		packet->data[4] = 0x00;
		packet->data[5] = 0x00;
		packet->data[6] = 0x00;
		packet->data[7] = 0x00;
		packet->data[8] = 0x00;
		packet->data[9] = 0x00;
		packet->data[10] = 0x00;
		packet->data[11] = 0x00;
		packet->len = 12;
		packet->pending = true;
		break;
	}

	xpad_try_sending_next_out_packet(xpad);

	spin_unlock_irqrestore(&xpad->odata_lock, flags);
}

/*
 * Light up the segment corresponding to the pad number on
 * Xbox 360 Controllers.
 */
static void xpad_identify_controller(struct usb_xpad *xpad)
{
	led_set_brightness(&xpad->led->led_cdev, (xpad->pad_nr % 4) + 2);
}

static void xpad_led_set(struct led_classdev *led_cdev,
			 enum led_brightness value)
{
	struct xpad_led *xpad_led = container_of(led_cdev,
						 struct xpad_led, led_cdev);

	xpad_send_led_command(xpad_led->xpad, value);
}

static int xpad_led_probe(struct usb_xpad *xpad)
{
	struct xpad_led *led;
	struct led_classdev *led_cdev;
	int error;

	if (xpad->xtype != XTYPE_XBOX360 && xpad->xtype != XTYPE_XBOX360W)
		return 0;

	xpad->led = led = kzalloc(sizeof(struct xpad_led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	xpad->pad_nr = ida_simple_get(&xpad_pad_seq, 0, 0, GFP_KERNEL);
	if (xpad->pad_nr < 0) {
		error = xpad->pad_nr;
		goto err_free_mem;
	}

	snprintf(led->name, sizeof(led->name), "xpad%d", xpad->pad_nr);
	led->xpad = xpad;

	led_cdev = &led->led_cdev;
	led_cdev->name = led->name;
	led_cdev->brightness_set = xpad_led_set;
	led_cdev->flags = LED_CORE_SUSPENDRESUME;

	error = led_classdev_register(&xpad->udev->dev, led_cdev);
	if (error)
		goto err_free_id;

	xpad_identify_controller(xpad);

	return 0;

err_free_id:
	ida_simple_remove(&xpad_pad_seq, xpad->pad_nr);
err_free_mem:
	kfree(led);
	xpad->led = NULL;
	return error;
}

static void xpad_led_disconnect(struct usb_xpad *xpad)
{
	struct xpad_led *xpad_led = xpad->led;

	if (xpad_led) {
		led_classdev_unregister(&xpad_led->led_cdev);
		ida_simple_remove(&xpad_pad_seq, xpad->pad_nr);
		kfree(xpad_led);
	}
}
#else
static int xpad_led_probe(struct usb_xpad *xpad) { return 0; }
static void xpad_led_disconnect(struct usb_xpad *xpad) { }
#endif

static int xpad_start_input(struct usb_xpad *xpad)
{
	int error;

	if (usb_submit_urb(xpad->irq_in, GFP_KERNEL))
		return -EIO;

	if (xpad->xtype == XTYPE_XBOXONE) {
		error = xpad_start_xbox_one(xpad);
		if (error) {
			usb_kill_urb(xpad->irq_in);
			return error;
		}
	}

	return 0;
}

static void xpad_stop_input(struct usb_xpad *xpad)
{
	usb_kill_urb(xpad->irq_in);
}

static void guncon3_start(struct usb_xpad *xpad)
{
    unsigned long flags;
    xpad->guncon3_key_sended=0;
    spin_lock_irqsave(&xpad->odata_lock, flags);


    /* Reset the sequence so we send out poweroff now */
    xpad->last_out_packet = -1;
    xpad_try_sending_next_out_packet(xpad);

    spin_unlock_irqrestore(&xpad->odata_lock, flags);
}
static void xpad360w_poweroff_controller(struct usb_xpad *xpad)
{
	unsigned long flags;
	struct xpad_output_packet *packet =
			&xpad->out_packets[XPAD_OUT_CMD_IDX];

	spin_lock_irqsave(&xpad->odata_lock, flags);

	packet->data[0] = 0x00;
	packet->data[1] = 0x00;
	packet->data[2] = 0x08;
	packet->data[3] = 0xC0;
	packet->data[4] = 0x00;
	packet->data[5] = 0x00;
	packet->data[6] = 0x00;
	packet->data[7] = 0x00;
	packet->data[8] = 0x00;
	packet->data[9] = 0x00;
	packet->data[10] = 0x00;
	packet->data[11] = 0x00;
	packet->len = 12;
	packet->pending = true;

	/* Reset the sequence so we send out poweroff now */
	xpad->last_out_packet = -1;
	xpad_try_sending_next_out_packet(xpad);

	spin_unlock_irqrestore(&xpad->odata_lock, flags);
}

static int xpad360w_start_input(struct usb_xpad *xpad)
{
	int error;

	error = usb_submit_urb(xpad->irq_in, GFP_KERNEL);
	if (error)
		return -EIO;

	/*
	 * Send presence packet.
	 * This will force the controller to resend connection packets.
	 * This is useful in the case we activate the module after the
	 * adapter has been plugged in, as it won't automatically
	 * send us info about the controllers.
	 */
	error = xpad_inquiry_pad_presence(xpad);
	if (error) {
		usb_kill_urb(xpad->irq_in);
		return error;
	}

	return 0;
}

static void xpad360w_stop_input(struct usb_xpad *xpad)
{
	usb_kill_urb(xpad->irq_in);

	/* Make sure we are done with presence work if it was scheduled */
	flush_work(&xpad->work);
}

static int xpad_open(struct input_dev *dev)
{
	struct usb_xpad *xpad = input_get_drvdata(dev);

	return xpad_start_input(xpad);
}

static void xpad_close(struct input_dev *dev)
{
	struct usb_xpad *xpad = input_get_drvdata(dev);

	xpad_stop_input(xpad);
}
static void guncon3_set_up_asb(struct input_dev *input_dev, signed short abs)
{
    struct usb_xpad *xpad = input_get_drvdata(input_dev);

    switch (abs) {
    case ABS_X:
    case ABS_Y:
        input_set_abs_params(input_dev, abs, -32768, 32767, 16, 128);
        break;
    case ABS_RX:
    case ABS_RY:	/* the two sticks */
    case ABS_HAT0X:
    case ABS_HAT0Y:	/* the d-pad (only if dpad is mapped to axes */
        input_set_abs_params(input_dev, abs, 0, 255, 0, 0);
        break;
    default:
        input_set_abs_params(input_dev, abs, 0, 0, 0, 0);
        break;
    }
}
static void xpad_set_up_abs(struct input_dev *input_dev, signed short abs)
{
	struct usb_xpad *xpad = input_get_drvdata(input_dev);

	switch (abs) {
	case ABS_X:
	case ABS_Y:
	case ABS_RX:
	case ABS_RY:	/* the two sticks */
		input_set_abs_params(input_dev, abs, -32768, 32767, 16, 128);
		break;
	case ABS_Z:
	case ABS_RZ:	/* the triggers (if mapped to axes) */
		if (xpad->xtype == XTYPE_XBOXONE)
			input_set_abs_params(input_dev, abs, 0, 1023, 0, 0);
		else
			input_set_abs_params(input_dev, abs, 0, 255, 0, 0);
		break;
	case ABS_HAT0X:
	case ABS_HAT0Y:	/* the d-pad (only if dpad is mapped to axes */
		input_set_abs_params(input_dev, abs, -1, 1, 0, 0);
		break;
	default:
		input_set_abs_params(input_dev, abs, 0, 0, 0, 0);
		break;
	}
}

static void xpad_deinit_input(struct usb_xpad *xpad)
{
	if (xpad->input_created) {
		xpad->input_created = false;
		xpad_led_disconnect(xpad);
		input_unregister_device(xpad->dev);
	}
}

static int xpad_init_input(struct usb_xpad *xpad)
{
	struct input_dev *input_dev;
	int i, error;

	input_dev = input_allocate_device();
	if (!input_dev)
		return -ENOMEM;

	xpad->dev = input_dev;
	input_dev->name = xpad->name;
	input_dev->phys = xpad->phys;
	usb_to_input_id(xpad->udev, &input_dev->id);
    //printk(KERN_INFO "xpad_init_input device:%d",xpad->xtype);

    if(xpad->xtype == XTYPE_GUNCON3)
    {
        input_dev->dev.parent = &xpad->intf->dev;
        input_set_drvdata(input_dev, xpad);
        input_dev->open = xpad_open;
        input_dev->close = xpad_close;
        for (i = 0; guncon3_sticks[i] >= 0; i++)
            guncon3_set_up_asb(input_dev, guncon3_sticks[i]);
        for (i = 0; guncon3_buttons[i] >= 0; i++)
            input_set_capability(input_dev, EV_KEY, guncon3_buttons[i]);
        error = input_register_device(xpad->dev);


        if (error)
            goto err_disconnect_led;

        xpad->input_created = true;
        return 0;

    }
	if (xpad->xtype == XTYPE_XBOX360W) {
		/* x360w controllers and the receiver have different ids */
		input_dev->id.product = 0x02a1;
	}

	input_dev->dev.parent = &xpad->intf->dev;

	input_set_drvdata(input_dev, xpad);

	if (xpad->xtype != XTYPE_XBOX360W) {
		input_dev->open = xpad_open;
		input_dev->close = xpad_close;
	}

    if (!(xpad->mapping & MAP_STICKS_TO_NULL)) {
		/* set up axes */
		for (i = 0; xpad_abs[i] >= 0; i++)
			xpad_set_up_abs(input_dev, xpad_abs[i]);
	}

	/* set up standard buttons */

    for (i = 0; xpad_common_btn[i] >= 0; i++)
		input_set_capability(input_dev, EV_KEY, xpad_common_btn[i]);

	/* set up model-specific ones */
	if (xpad->xtype == XTYPE_XBOX360 || xpad->xtype == XTYPE_XBOX360W ||
	    xpad->xtype == XTYPE_XBOXONE) {
		for (i = 0; xpad360_btn[i] >= 0; i++)
			input_set_capability(input_dev, EV_KEY, xpad360_btn[i]);
	} else {
       for (i = 0; xpad_btn[i] >= 0; i++)
			input_set_capability(input_dev, EV_KEY, xpad_btn[i]);

        }

	if (xpad->mapping & MAP_DPAD_TO_BUTTONS) {
		for (i = 0; xpad_btn_pad[i] >= 0; i++)
			input_set_capability(input_dev, EV_KEY,
					     xpad_btn_pad[i]);
	}

	/*
	 * This should be a simple else block. However historically
	 * xbox360w has mapped DPAD to buttons while xbox360 did not. This
	 * made no sense, but now we can not just switch back and have to
	 * support both behaviors.
	 */
	if (!(xpad->mapping & MAP_DPAD_TO_BUTTONS) ||
	    xpad->xtype == XTYPE_XBOX360W) {
		for (i = 0; xpad_abs_pad[i] >= 0; i++)
			xpad_set_up_abs(input_dev, xpad_abs_pad[i]);
	}

	if (xpad->mapping & MAP_TRIGGERS_TO_BUTTONS) {
		for (i = 0; xpad_btn_triggers[i] >= 0; i++)
			input_set_capability(input_dev, EV_KEY,
					     xpad_btn_triggers[i]);
	} else {
        for (i = 0; xpad_abs_triggers[i] >= 0; i++)
			xpad_set_up_abs(input_dev, xpad_abs_triggers[i]);

	}
    error = xpad_init_ff(xpad);
	if (error)
		goto err_free_input;

	error = xpad_led_probe(xpad);
	if (error)
		goto err_destroy_ff;

	error = input_register_device(xpad->dev);
	if (error)
		goto err_disconnect_led;

	xpad->input_created = true;
	return 0;

err_disconnect_led:
	xpad_led_disconnect(xpad);
err_destroy_ff:
	input_ff_destroy(input_dev);
err_free_input:
	input_free_device(input_dev);
	return error;
}

static int xpad_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_xpad *xpad;
	struct usb_endpoint_descriptor *ep_irq_in, *ep_irq_out;
	int i, error;

	if (intf->cur_altsetting->desc.bNumEndpoints != 2)
		return -ENODEV;

	for (i = 0; xpad_device[i].idVendor; i++) {
		if ((le16_to_cpu(udev->descriptor.idVendor) == xpad_device[i].idVendor) &&
		    (le16_to_cpu(udev->descriptor.idProduct) == xpad_device[i].idProduct))
			break;
	}

	xpad = kzalloc(sizeof(struct usb_xpad), GFP_KERNEL);
	if (!xpad)
		return -ENOMEM;

	usb_make_path(udev, xpad->phys, sizeof(xpad->phys));
	strlcat(xpad->phys, "/input0", sizeof(xpad->phys));

	xpad->idata = usb_alloc_coherent(udev, XPAD_PKT_LEN,
					 GFP_KERNEL, &xpad->idata_dma);
	if (!xpad->idata) {
		error = -ENOMEM;
		goto err_free_mem;
	}

	xpad->irq_in = usb_alloc_urb(0, GFP_KERNEL);
	if (!xpad->irq_in) {
		error = -ENOMEM;
		goto err_free_idata;
	}

	xpad->udev = udev;
	xpad->intf = intf;
	xpad->mapping = xpad_device[i].mapping;
	xpad->xtype = xpad_device[i].xtype;
    if(xpad->xtype==XTYPE_GUNCON3)
    {
        //printk(KERN_INFO "1.- GunCon3 identificado\n");

    }
	xpad->name = xpad_device[i].name;
	INIT_WORK(&xpad->work, xpad_presence_work);
    //printk(KERN_INFO "2.- Work init\n");
	if (xpad->xtype == XTYPE_UNKNOWN) {
		if (intf->cur_altsetting->desc.bInterfaceClass == USB_CLASS_VENDOR_SPEC) {
			if (intf->cur_altsetting->desc.bInterfaceProtocol == 129)
				xpad->xtype = XTYPE_XBOX360W;
			else if (intf->cur_altsetting->desc.bInterfaceProtocol == 208)
				xpad->xtype = XTYPE_XBOXONE;
			else
				xpad->xtype = XTYPE_XBOX360;
		} else {
			xpad->xtype = XTYPE_XBOX;
		}

		if (dpad_to_buttons)
			xpad->mapping |= MAP_DPAD_TO_BUTTONS;
		if (triggers_to_buttons)
			xpad->mapping |= MAP_TRIGGERS_TO_BUTTONS;
		if (sticks_to_null)
			xpad->mapping |= MAP_STICKS_TO_NULL;
	}

	if (xpad->xtype == XTYPE_XBOXONE &&
	    intf->cur_altsetting->desc.bInterfaceNumber != 0) {
		/*
		 * The Xbox One controller lists three interfaces all with the
		 * same interface class, subclass and protocol. Differentiate by
		 * interface number.
		 */
		error = -ENODEV;
		goto err_free_in_urb;
	}
    //printk(KERN_INFO "Preparando endpoints\n");
	ep_irq_in = ep_irq_out = NULL;

	for (i = 0; i < 2; i++) {
		struct usb_endpoint_descriptor *ep =
				&intf->cur_altsetting->endpoint[i].desc;

		if (usb_endpoint_xfer_int(ep)) {
			if (usb_endpoint_dir_in(ep))
				ep_irq_in = ep;
			else
				ep_irq_out = ep;
		}
	}

	if (!ep_irq_in || !ep_irq_out) {
        //printk(KERN_INFO "Error al abrir endpoint\n");
		error = -ENODEV;
		goto err_free_in_urb;
	}
    //printk(KERN_INFO "3.-Init output\n");
	error = xpad_init_output(intf, xpad, ep_irq_out);
	if (error)
    {
        //printk(KERN_INFO "Error al iniciar output\n");
		goto err_free_in_urb;
    }
    if(xpad->xtype==XTYPE_GUNCON3)
    {
        //printk(KERN_INFO "guncon usb fill int urb\n");
        usb_fill_int_urb(xpad->irq_in, udev,
                 usb_rcvintpipe(udev, ep_irq_in->bEndpointAddress),
                 xpad->idata, XPAD_PKT_LEN_GUNCON3_IN, xpad_irq_in,
                 xpad, ep_irq_in->bInterval);
        //printk(KERN_INFO "bEndpointAddres %d, XPAD_PKT_LEN %d, bInterval %d\n",
        //       ep_irq_in->bEndpointAddress,XPAD_PKT_LEN_GUNCON3_IN,ep_irq_in->bInterval);

    }
    else
    {
	usb_fill_int_urb(xpad->irq_in, udev,
			 usb_rcvintpipe(udev, ep_irq_in->bEndpointAddress),
			 xpad->idata, XPAD_PKT_LEN, xpad_irq_in,
			 xpad, ep_irq_in->bInterval);
    }
    xpad->irq_in->transfer_dma = xpad->idata_dma;
	xpad->irq_in->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	usb_set_intfdata(intf, xpad);

	if (xpad->xtype == XTYPE_XBOX360W) {
		/*
		 * Submit the int URB immediately rather than waiting for open
		 * because we get status messages from the device whether
		 * or not any controllers are attached.  In fact, it's
		 * exactly the message that a controller has arrived that
		 * we're waiting for.
		 */
		error = xpad360w_start_input(xpad);
		if (error)
			goto err_deinit_output;
		/*
		 * Wireless controllers require RESET_RESUME to work properly
		 * after suspend. Ideally this quirk should be in usb core
		 * quirk list, but we have too many vendors producing these
		 * controllers and we'd need to maintain 2 identical lists
		 * here in this driver and in usb core.
		 */
		udev->quirks |= USB_QUIRK_RESET_RESUME;
	} else {
        //printk(KERN_INFO "Iniciado input\n");
		error = xpad_init_input(xpad);
		if (error)
        {
            //printk(KERN_INFO "Error al abrir input\n");
			goto err_deinit_output;
        }
     }
    //printk(KERN_INFO "prepare send key\n");
   guncon3_start(xpad);
	return 0;

err_deinit_output:
	xpad_deinit_output(xpad);
err_free_in_urb:
	usb_free_urb(xpad->irq_in);
err_free_idata:
	usb_free_coherent(udev, XPAD_PKT_LEN, xpad->idata, xpad->idata_dma);
err_free_mem:
	kfree(xpad);
	return error;
}

static void xpad_disconnect(struct usb_interface *intf)
{
	struct usb_xpad *xpad = usb_get_intfdata(intf);

	if (xpad->xtype == XTYPE_XBOX360W)
		xpad360w_stop_input(xpad);

	xpad_deinit_input(xpad);

	/*
	 * Now that both input device and LED device are gone we can
	 * stop output URB.
	 */
	xpad_stop_output(xpad);

	xpad_deinit_output(xpad);

	usb_free_urb(xpad->irq_in);
    if(xpad->xtype==XTYPE_GUNCON3)
    {
        usb_free_coherent(xpad->udev, XPAD_PKT_LEN_GUNCON3_IN,
                xpad->idata, xpad->idata_dma);

    }
    else {

	usb_free_coherent(xpad->udev, XPAD_PKT_LEN,
			xpad->idata, xpad->idata_dma);
    }
	kfree(xpad);

	usb_set_intfdata(intf, NULL);
}

static int xpad_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct usb_xpad *xpad = usb_get_intfdata(intf);
	struct input_dev *input = xpad->dev;

	if (xpad->xtype == XTYPE_XBOX360W) {
		/*
		 * Wireless controllers always listen to input so
		 * they are notified when controller shows up
		 * or goes away.
		 */
		xpad360w_stop_input(xpad);

		/*
		 * The wireless adapter is going off now, so the
		 * gamepads are going to become disconnected.
		 * Unless explicitly disabled, power them down
		 * so they don't just sit there flashing.
		 */
		if (auto_poweroff && xpad->pad_present)
			xpad360w_poweroff_controller(xpad);
	} else {
		mutex_lock(&input->mutex);
		if (input->users)
			xpad_stop_input(xpad);
		mutex_unlock(&input->mutex);
	}

	xpad_stop_output(xpad);

	return 0;
}

static int xpad_resume(struct usb_interface *intf)
{
	struct usb_xpad *xpad = usb_get_intfdata(intf);
	struct input_dev *input = xpad->dev;
	int retval = 0;

	if (xpad->xtype == XTYPE_XBOX360W) {
		retval = xpad360w_start_input(xpad);
	} else {
		mutex_lock(&input->mutex);
		if (input->users) {
			retval = xpad_start_input(xpad);
		} else if (xpad->xtype == XTYPE_XBOXONE) {
			/*
			 * Even if there are no users, we'll send Xbox One pads
			 * the startup sequence so they don't sit there and
			 * blink until somebody opens the input device again.
			 */
			retval = xpad_start_xbox_one(xpad);
		}
		mutex_unlock(&input->mutex);
	}

	return retval;
}

static struct usb_driver xpad_driver = {
	.name		= "xpad",
	.probe		= xpad_probe,
	.disconnect	= xpad_disconnect,
	.suspend	= xpad_suspend,
	.resume		= xpad_resume,
	.id_table	= xpad_table,
};

module_usb_driver(xpad_driver);

MODULE_AUTHOR("Marko Friedemann <mfr@bmx-chemnitz.de>");
MODULE_DESCRIPTION("X-Box pad driver");
MODULE_LICENSE("GPL");
