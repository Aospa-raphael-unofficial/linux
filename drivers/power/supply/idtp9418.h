#ifndef __IDTP9148_H__
#define __IDTP9148_H__

#define IDT_DRIVER_NAME "idtp9418"
#define IDT_I2C_ADDR 0x3b

#define HSCLK 60000

#define ADJUST_METE_MV 35
#define IDTP9220_DELAY 2000
#define CHARGING_FULL 100
#define CHARGING_NEED 95

/* status low regiter bits define */
#define STATUS_VOUT_ON (1 << 7)
#define STATUS_VOUT_OFF (1 << 6)
#define STATUS_TX_DATA_RECV (1 << 4)
#define STATUS_OV_TEMP (1 << 2)
#define STATUS_OV_VOL (1 << 1)
#define STATUS_OV_CURR (1 << 0)

#define OVER_EVENT_OCCUR (STATUS_OV_TEMP | STATUS_OV_VOL | STATUS_OV_CURR)
#define TOGGLE_LDO_ON_OFF (1 << 1)

/* interrupt register bits define */
#define INT_RXREADY (1 << 22)
#define INT_POWER_LOSS (1 << 21)
#define INT_OVERCURRENT_HIGH (1 << 20)
#define RENEG_FAIL (1 << 19)
#define RENEG_SUCCESS (1 << 18)
#define INT_RPP_READ (1 << 17)
#define INT_EXT5V_FAIL (1 << 16)
#define INT_VSWITCH_FAIL (1 << 15)
#define INT_SLEEPMODE (1 << 14)
#define INT_IDAUTH_SUCESS (1 << 13)
#define INT_IDAUTH_FAIL (1 << 12)
#define INT_SEND_SUCESS (1 << 11)
#define INT_SEND_TIMEOUT (1 << 10)
#define INT_AUTH_SUCESS (1 << 9)
#define INT_AUTH_FAIL (1 << 8)
#define INT_VOUT_OFF (1 << 7)
#define INT_VOUT_ON (1 << 6)
#define INT_MODE_CHANGE (1 << 5)
#define INT_TX_DATA_RECV (1 << 4)
#define INT_VSWITCH_SUCESS (1 << 3)
#define INT_OV_TEMP (1 << 2)
#define INT_OV_VOL (1 << 1)
#define INT_OV_CURR (1 << 0)

/*TRX int bits define*/
#define INT_GET_BLE_ADDR (1 << 8)
#define INT_INIT_TX (1 << 7)
#define INT_GET_DPING (1 << 6)
#define INT_GET_PPP (1 << 5)
#define INT_GET_CFG (1 << 4)
#define INT_GET_ID (1 << 3)
#define INT_GET_SS (1 << 2)
#define INT_START_DPING (1 << 1)
#define INT_EPT_TYPE (1 << 0)

#define TX_TOGGLE BIT(6)   // toggle work mode(148K or 190K)
#define TX_FOD_EN BIT(5)   // enable FOD
#define TX_WD BIT(4)       // enable WD
#define TX_SEND_FSK BIT(3) // SEND_FSK
#define TX_DIS BIT(2)      // disable tx
#define TX_CLRINT BIT(1)   // clr int
#define TX_EN BIT(0)       // enable tx mode

/* used registers define */
#define REG_CHIP_ID_L 0x0000
#define REG_CHIP_ID_H 0x0001
#define REG_CHIP_REV 0x0002
#define REG_CTM_ID 0x0003
#define REG_OTPFWVER_ADDR 0x0004 // OTP firmware version
#define REG_EPRFWVER_ADDR 0x001c // EEPROM firmware version
#define REG_SYS_INT_CLR 0x0028   // int clear
#define REG_SYS_INT 0x0030       // interrupt
#define REG_STATUS_L 0x0034
#define REG_STATUS_H 0x0035
#define REG_INTR_L 0x0036
#define REG_INTR_H 0x0037
#define REG_INTR_EN_L 0x0038
#define REG_INTR_EN_H 0x0039
#define REG_CHG_STATUS 0x003A
#define REG_ADC_VOUT_L 0x003C
#define REG_ADC_VOUT_H 0x003D
#define REG_VOUT_SET 0x003E
#define REG_VRECT_ADJ 0x003F
#define REG_ADC_VRECT 0x0040
#define REG_RX_LOUT_L 0x0044
#define REG_RX_LOUT_H 0x0045
#define REG_RX_DIE_TEMP 0x0046
#define REG_FREQ_ADDR 0x0048 // Operating Frequency, Fop(KHz) = 64 * 6000 /value * 256)
#define REG_ILIM_SET 0x004A
#define REG_SIGNAL_STRENGTH 0x004B
#define REG_WPC_MODE 0x004D
#define REG_SSCMND 0x004e // Command Register, COM (0x4E)
#define REG_RX_RESET 0x004F
#define REG_PROPPKT 0x0050      // Proprietary Packet Header Register, PPP_Header (0x50)
#define REG_PPPDATA 0x0051      // PPP Data Value Register(0X51, 0x52, 0x53, 0x54, 0x55)
#define REG_SSINTCLR 0x0056     // Interrupt Clear Registers, INT_Clear_L (0x56)
#define REG_BCHEADER 0x0058     // Back Channel Packet Register (0x58)
#define REG_BCDATA 0x0059       // Back Channel Packet Register (0x59, 0x5A, 0x5B, 0x5C)
#define REG_FC_VOLTAGE_L 0x0078 // Fast Charging Voltage Register
#define REG_FC_VOLTAGE_H 0x0079
#define REG_REGULATOR_L 0x000C
#define REG_REGULATOR_H 0x000d
#define REG_POWER_MAX 0x0084 // Get the TX power on EPP mode.
#define REG_TX_TYPE 0x00A2   // Get the TX type.
#define REG_BLE_FLAG 0x00A4  // Get the rx ble flag.
#define REG_CEP 0x00A5       // Get the CEP.
#define REG_RPP 0x00A6       // Get the RPP.
#define REG_MAX_POWER 0x008E
#define REG_OCP_CONFIG 0x00F1
#define REG_TEMPTER 0x00F2

/*add for reverse charge*/
#define REG_EPT_TYPE 0x0074
#define REG_TX_CMD 0x0076
#define REG_TX_DATA 0x0078
#define REG_WROK_MODE 0x007B
#define REG_VRECT_TAEGET 0x0090
#define REG_FOD_LOW 0x0092
#define REG_FOD_HIGH 0x0093
#define REG_MAC_ADDR 0x00be

#define REG_REVERSE_IIN_LOW 0x006e
#define REG_REVERSE_IIN_HIGH 0x006f
#define REG_REVERSE_VIN_LOW 0x0070
#define REG_REVERSE_VIN_HIGH 0x0071
#define REG_REVERSE_TEMP 0x007a

#define EPT_POCP BIT(15)
#define EPT_OTP BIT(14)
#define EPT_FOD BIT(13)
#define EPT_LVP BIT(12)
#define EPT_OVP BIT(11)
#define EPT_OCP BIT(10)
#define EPT_RPP_TIMEOUT BIT(9)
#define EPT_CEP_TIMEOUT BIT(8)
#define EPT_CMD BIT(0)

#define REG_EXTERNAL_5V 0x00dc // external 5v enable

// RX -> TX
#define PROPRIETARY18 0x18
#define PROPRIETARY28 0x28
#define PROPRIETARY38 0x38
#define PROPRIETARY48 0x48
#define PROPRIETARY58 0x58

// bitmap for customer command
#define BC_NONE 0x00
#define BC_SET_FREQ 0x03
#define BC_GET_FREQ 0x04
#define BC_READ_FW_VER 0x05
#define BC_READ_Iin 0x06
#define BC_READ_Vin 0x07
#define BC_SET_Vin 0x0a

#define BC_ADAPTER_TYPE 0x0b
#define BC_RESET 0x0c
#define BC_READ_I2C 0x0d
#define BC_WRITE_I2C 0x0e
#define BC_VI2C_INIT 0x10
#define BC_RX_ID_AUTH 0x3b
#define BC_TX_COMPATIBLE_HWID 0x3f
#define BC_TX_HWID 0x4c
#define BC_TX_TOGGLE 0xc4
#define CMD_GET_BLEMAC_2_0 0xb6
#define CMD_GET_BLEMAC_5_3 0xb7
#define CMD_FACOTRY_REVERSE 0x30

#define NORMAL_MODE 0x1
#define TAPER_MODE 0x2
#define FULL_MODE 0x3
#define RECHG_MODE 0x4

// fw version to update
#define FW_VERSION 0x1

// add for crc verify
#define CRC_VERIFY_LOW 0xB5
#define CRC_VERIFY_HIGH 0xA9

// add for reverse fod
#define REVERSE_FOD 500

/*CLRPOWERGOOD BIT(10), upper byte is 0x8*/
#define CLRPOWERGOOD 0x04
// bitmap for status flags
// 1: indicates a pending interrupt for LDO Vout state change – from OFF to ON
#define VOUTCHANGED BIT(7) // Stat_Vout_ON
// 1: indicates a pending interrupt for TX Data Received. (Change from “No Received Data” state to “Data Received” state)
#define TXDATARCVD BIT(4) // TX Data Received

// bitmap for SSCmnd register 0x4e
#define VSWITCH BIT(7)
// If AP sets this bit to "1" then IDTP9220 M0 clears the interrupt corresponding to the bit(s) which has a value of “1”
#define CLRINT BIT(5) // Clear Interrupt
// If AP sets this bit to "1" then IDTP9220 M0 toggles LDO output once (from on to off, or from off to on), and then sets this bit to “0”
#define LDOTGL BIT(1) // Toggle LDO On/OFF
// If AP sets this bit to “1” then IDTP9220 M0 sends the Proprietary Packet
#define SENDPROPP BIT(0) //  SEND RX Data

#define SEND_DEVICE_AUTH BIT(2)

#endif
