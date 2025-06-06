/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MacBook (Pro) SPI keyboard and touchpad driver
 *
 * Copyright (c) 2015-2019 Federico Lorenzi
 * Copyright (c) 2017-2019 Ronald Tschal盲r
 */

/*******************************************************************************
* Copyright 2011-2012, Cypress Semiconductor Corporation.  All rights reserved.
* You may use this file only in accordance with the license, terms, conditions,
* disclaimers, and limitations in the end user license agreement accompanying
* the software package with which this file was provided.
********************************************************************************/

#ifndef __CYBTLDR_COMMAND_H__
#define __CYBTLDR_COMMAND_H__


/* Maximum number of bytes to allocate for a single command.  */
#define MAX_COMMAND_SIZE 512


//STANDARD PACKET FORMAT:
// Multi byte entries are encoded in LittleEndian.
/*******************************************************************************
* [1-byte] [1-byte ] [2-byte] [n-byte] [ 2-byte ] [1-byte]
* [ SOP  ] [Command] [ Size ] [ Data ] [Checksum] [ EOP  ]
*******************************************************************************/


/* The first byte of any boot loader command. */
#define CMD_START               0x01
/* The last byte of any boot loader command. */
#define CMD_STOP                0x17
/* The minimum number of bytes in a bootloader command. */
#define BASE_CMD_SIZE           0x07

/* Command identifier for verifying the checksum value of the bootloadable project. */
#define CMD_VERIFY_CHECKSUM     0x31
/* Command identifier for getting the number of flash rows in the target device. */
#define CMD_GET_FLASH_SIZE      0x32
/* Command identifier for getting info about the app status. This is only supported on multi app bootloader. */
#define CMD_GET_APP_STATUS      0x33
/* Command identifier for reasing a row of flash data from the target device. */
#define CMD_ERASE_ROW           0x34
/* Command identifier for making sure the bootloader host and bootloader are in sync. */
#define CMD_SYNC                0x35
/* Command identifier for setting the active application. This is only supported on multi app bootloader. */
#define CMD_SET_ACTIVE_APP      0x36
/* Command identifier for sending a block of data to the bootloader without doing anything with it yet. */
#define CMD_SEND_DATA           0x37
/* Command identifier for starting the boot loader.  All other commands ignored until this is sent. */
#define CMD_ENTER_BOOTLOADER    0x38
/* Command identifier for programming a single row of flash. */
#define CMD_PROGRAM_ROW         0x39
/* Command identifier for verifying the contents of a single row of flash. */
#define CMD_VERIFY_ROW          0x3A
/* Command identifier for exiting the bootloader and restarting the target program. */
#define CMD_EXIT_BOOTLOADER     0x3B


/******************************************************************************
 *    HOST ERROR CODES
 ******************************************************************************
 *
 * Different return codes from the bootloader host.  Functions are not
 * limited to these values, but are encuraged to use them when returning
 * standard error values.
 *
 * 0 is successful, all other values indicate a failure.
 *****************************************************************************/
/* Completed successfully */
#ifndef CYRET_SUCCESS
#define CYRET_SUCCESS           0x00
#endif
/* File is not accessible */
#define CYRET_ERR_FILE          0x01
/* Reached the end of the file */
#define CYRET_ERR_EOF           0x02
/* The amount of data available is outside the expected range */
#define CYRET_ERR_LENGTH        0x03
/* The data is not of the proper form */
#define CYRET_ERR_DATA          0x04
/* The command is not recognized */
#define CYRET_ERR_CMD           0x05
/* The expected device does not match the detected device */
#define CYRET_ERR_DEVICE        0x06
/* The bootloader version detected is not supported */
#define CYRET_ERR_VERSION       0x07
/* The checksum does not match the expected value */
#define CYRET_ERR_CHECKSUM      0x08
/* The flash array is not valid */
#define CYRET_ERR_ARRAY         0x09
/* The flash row is not valid */
#define CYRET_ERR_ROW           0x0A
/* The bootloader is not ready to process data */
#define CYRET_ERR_BTLDR         0x0B
/* The application is currently marked as active */
#define CYRET_ERR_ACTIVE        0x0C
/* An unknown error occured */
#define CYRET_ERR_UNK           0x0F
/* The operation was aborted */
#define CYRET_ABORT             0xFF

/* The communications object reported an error */
#define CYRET_ERR_COMM_MASK     0x2000
/* The bootloader reported an error */
#define CYRET_ERR_BTLDR_MASK    0x4000

/* Maximum number of bytes to allocate for a single row.  */
/* NB: Rows should have a max of 592 chars (2-arrayID, 4-rowNum, 4-len, 576-data, 2-checksum, 4-newline) */
#define MAX_BUFFER_SIZE 768


/*
 * This enum defines the different types of checksums that can be
 * used by the bootloader for ensuring data integrety.
 */
typedef enum {
    /* Checksum type is a basic inverted summation of all bytes */
    SUM_CHECKSUM = 0x00,
    /* 16-bit CRC checksum using the CCITT implementation */
    CRC_CHECKSUM = 0x01,
} CyBtldr_left_ChecksumType;

/* Variable used to store the currently selected packet checksum type */
extern CyBtldr_left_ChecksumType CyBtldr_left_Checksum;


/*******************************************************************************
* Function Name: CyBtldr_leftComputeChecksum
********************************************************************************
* Summary:
*   Computes the 2byte checksum for the provided command data.  The checksum is
*   the 2's complement of the 1-byte sum of all bytes.
*
* Parameters:
*   buf  - The data to compute the checksum on
*   size - The number of bytes contained in buf.
*
* Returns:
*   The checksum for the provided data.
*
*******************************************************************************/
unsigned short CyBtldr_left_ComputeChecksum(unsigned char *buf, unsigned long size);

/*******************************************************************************
* Function Name: CyBtldr_leftParseDefaultCmdResult
********************************************************************************
* Summary:
*   Parses the output from any command that returns the default result packet
*   data.  The default result is just a status byte
*
* Parameters:
*   cmdBuf  - The preallocated buffer to store command data in.
*   cmdSize - The number of bytes in the command.
*   status  - The status code returned by the bootloader.
*
* Returns:
*   CYRET_SUCCESS    - The command was constructed successfully
*   CYRET_ERR_LENGTH - The packet does not contain enough data
*   CYRET_ERR_DATA   - The packet's contents are not correct
*
*******************************************************************************/
int CyBtldr_left_ParseDefaultCmdResult(unsigned char *cmdBuf, unsigned long cmdSize, unsigned char *status);

/*******************************************************************************
* Function Name: CyBtldr_leftCreateEnterBootLoaderCmd
********************************************************************************
* Summary:
*   Creates the command used to startup the bootloader.
*   NB: This command must be sent before the bootloader will respond to any
*       other command.
*
* Parameters:
*   protect - The flash protection settings.
*   cmdBuf  - The preallocated buffer to store command data in.
*   cmdSize - The number of bytes in the command.
*   resSize - The number of bytes expected in the bootloader's response packet.
*
* Returns:
*   CYRET_SUCCESS  - The command was constructed successfully
*
*******************************************************************************/
extern int CyBtldr_left_CreateEnterBootLoaderCmd(unsigned char *cmdBuf, unsigned long *cmdSize, unsigned long *resSize);

/*******************************************************************************
* Function Name: CyBtldr_leftParseEnterBootLoaderCmdResult
********************************************************************************
* Summary:
*   Parses the output from the EnterBootLoader command to get the resultant
*   data.
*
* Parameters:
*   cmdBuf     - The buffer containing the output from the bootloader.
*   cmdSize    - The number of bytes in cmdBuf.
*   siliconId  - The silicon ID of the device being communicated with.
*   siliconRev - The silicon Revision of the device being communicated with.
*   blVersion  - The bootloader version being communicated with.
*   status     - The status code returned by the bootloader.
*
* Returns:
*   CYRET_SUCCESS    - The command was constructed successfully
*   CYRET_ERR_LENGTH - The packet does not contain enough data
*   CYRET_ERR_DATA   - The packet's contents are not correct
*
*******************************************************************************/
extern int CyBtldr_left_ParseEnterBootLoaderCmdResult(unsigned char *cmdBuf, unsigned long cmdSize, unsigned long *siliconId, unsigned char *siliconRev, unsigned long *blVersion, unsigned char *status);

/*******************************************************************************
* Function Name: CyBtldr_left_CreateExitBootLoaderCmd
********************************************************************************
* Summary:
*   Creates the command used to stop communicating with the boot loader and to
*   trigger the target device to restart, running the new bootloadable
*   application.
*
* Parameters:
*   resetType - The type of reset to perform (0 = Reset, 1 = Direct Call).
*   cmdBuf    - The preallocated buffer to store command data in.
*   cmdSize   - The number of bytes in the command.
*   resSize   - The number of bytes expected in the bootloader's response packet.
*
* Returns:
*   CYRET_SUCCESS  - The command was constructed successfully
*
*******************************************************************************/
extern int CyBtldr_left_CreateExitBootLoaderCmd(unsigned char resetType, unsigned char *cmdBuf, unsigned long *cmdSize, unsigned long *resSize);

/*******************************************************************************
* Function Name: CyBtldr_left_CreateProgramRowCmd
********************************************************************************
* Summary:
*   Creates the command used to program a single flash row.
*
* Parameters:
*   arrayId - The array id to program.
*   rowNum  - The row number to program.
*   buf     - The buffer of data to program into the flash row.
*   size    - The number of bytes in data for the row.
*   cmdBuf  - The preallocated buffer to store command data in.
*   cmdSize - The number of bytes in the command.
*   resSize - The number of bytes expected in the bootloader's response packet.
*
* Returns:
*   CYRET_SUCCESS  - The command was constructed successfully
*
*******************************************************************************/
extern int CyBtldr_left_CreateProgramRowCmd(unsigned char arrayId, unsigned short rowNum, unsigned char *buf, unsigned short size, unsigned char *cmdBuf, unsigned long *cmdSize, unsigned long *resSize);

/*******************************************************************************
* Function Name: CyBtldr_left_ParseProgramRowCmdResult
********************************************************************************
* Summary:
*   Parses the output from the ProgramRow command to get the resultant
*   data.
*
* Parameters:
*   cmdBuf  - The preallocated buffer to store command data in.
*   cmdSize - The number of bytes in the command.
*   status  - The status code returned by the bootloader.
*
* Returns:
*   CYRET_SUCCESS    - The command was constructed successfully
*   CYRET_ERR_LENGTH - The packet does not contain enough data
*   CYRET_ERR_DATA   - The packet's contents are not correct
*
*******************************************************************************/
extern int CyBtldr_left_ParseProgramRowCmdResult(unsigned char *cmdBuf, unsigned long cmdSize, unsigned char *status);

/*******************************************************************************
* Function Name: CyBtldr_left_CreateVerifyRowCmd
********************************************************************************
* Summary:
*   Creates the command used to verify that the contents of flash match the
*   provided row data.
*
* Parameters:
*   arrayId - The array id to verify.
*   rowNum  - The row number to verify.
*   cmdBuf  - The preallocated buffer to store command data in.
*   cmdSize - The number of bytes in the command.
*   resSize - The number of bytes expected in the bootloader's response packet.
*
* Returns:
*   CYRET_SUCCESS  - The command was constructed successfully
*
*******************************************************************************/
extern int CyBtldr_left_CreateVerifyRowCmd(unsigned char arrayId, unsigned short rowNum, unsigned char *cmdBuf, unsigned long *cmdSize, unsigned long *resSize);

/*******************************************************************************
* Function Name: CyBtldr_left_ParseVerifyRowCmdResult
********************************************************************************
* Summary:
*   Parses the output from the VerifyRow command to get the resultant
*   data.
*
* Parameters:
*   cmdBuf   - The preallocated buffer to store command data in.
*   cmdSize  - The number of bytes in the command.
*   checksum - The checksum from the row to verify.
*   status   - The status code returned by the bootloader.
*
* Returns:
*   CYRET_SUCCESS    - The command was constructed successfully
*   CYRET_ERR_LENGTH - The packet does not contain enough data
*   CYRET_ERR_DATA   - The packet's contents are not correct
*
*******************************************************************************/
extern int CyBtldr_left_ParseVerifyRowCmdResult(unsigned char *cmdBuf, unsigned long cmdSize, unsigned char *checksum, unsigned char *status);

/*******************************************************************************
* Function Name: CyBtldr_left_CreateGetFlashSizeCmd
********************************************************************************
* Summary:
*   Creates the command used to retreive the number of flash rows in the device.
*
* Parameters:
*   arrayId - The array ID to get the flash size of.
*   cmdBuf  - The preallocated buffer to store command data in.
*   cmdSize - The number of bytes in the command.
*   resSize - The number of bytes expected in the bootloader's response packet.
*
* Returns:
*   CYRET_SUCCESS  - The command was constructed successfully
*
*******************************************************************************/
extern int CyBtldr_left_CreateGetFlashSizeCmd(unsigned char arrayId, unsigned char *cmdBuf, unsigned long *cmdSize, unsigned long *resSize);

/*******************************************************************************
* Function Name: CyBtldr_left_ParseGetFlashSizeCmdResult
********************************************************************************
* Summary:
*   Parses the output from the GetFlashSize command to get the resultant
*   data.
*
* Parameters:
*   cmdBuf   - The preallocated buffer to store command data in.
*   cmdSize  - The number of bytes in the command.
*   startRow - The first available row number in the flash array.
*   endRow   - The last available row number in the flash array.
*   status   - The status code returned by the bootloader.
*
* Returns:
*   CYRET_SUCCESS    - The command was constructed successfully
*   CYRET_ERR_LENGTH - The packet does not contain enough data
*   CYRET_ERR_DATA   - The packet's contents are not correct
*
*******************************************************************************/
extern int CyBtldr_left_ParseGetFlashSizeCmdResult(unsigned char *cmdBuf, unsigned long cmdSize, unsigned short *startRow, unsigned short *endRow, unsigned char *status);

/*******************************************************************************
* Function Name: CyBtldr_left_CreateSendDataCmd
********************************************************************************
* Summary:
*   Creates the command used to send a block of data to the target.
*
* Parameters:
*   buf     - The buffer of data data to program into the flash row.
*   size    - The number of bytes in data for the row.
*   cmdBuf  - The preallocated buffer to store command data in.
*   cmdSize - The number of bytes in the command.
*   resSize - The number of bytes expected in the bootloader's response packet.
*
* Returns:
*   CYRET_SUCCESS  - The command was constructed successfully
*
*******************************************************************************/
extern int CyBtldr_left_CreateSendDataCmd(unsigned char *buf, unsigned short size, unsigned char *cmdBuf, unsigned long *cmdSize, unsigned long *resSize);

/*******************************************************************************
* Function Name: CyBtldr_left_ParseSendDataCmdResult
********************************************************************************
* Summary:
*   Parses the output from the SendData command to get the resultant
*   data.
*
* Parameters:
*   cmdBuf  - The preallocated buffer to store command data in.
*   cmdSize - The number of bytes in the command.
*   status  - The status code returned by the bootloader.
*
* Returns:
*   CYRET_SUCCESS    - The command was constructed successfully
*   CYRET_ERR_LENGTH - The packet does not contain enough data
*   CYRET_ERR_DATA   - The packet's contents are not correct
*
*******************************************************************************/
extern int CyBtldr_left_ParseSendDataCmdResult(unsigned char *cmdBuf, unsigned long cmdSize, unsigned char *status);
#endif
