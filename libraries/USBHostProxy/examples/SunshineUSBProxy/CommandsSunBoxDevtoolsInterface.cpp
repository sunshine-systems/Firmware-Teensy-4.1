#include "CommandsSunBoxDevtoolsInterface.h"
#include "USBHostDriver.h"
#include "HIDMouseDescriptorHandler.h"
#include "SunBoxEEPROM.h"
#include "SunBoxStartup.h"

// Global state for vendor monitoring
static bool vendorMonitorEnabled = false;
static uint32_t vendorTransferCount = 0;

CommandsSunBoxDevtoolsInterface::CommandsSunBoxDevtoolsInterface()
    : usbHostDriver(nullptr), hidHandler(nullptr), debugEnabled(false) {
}

void CommandsSunBoxDevtoolsInterface::begin() {
    // Get current debug state from SunBoxStartup (already loaded at startup)
    debugEnabled = SunBoxStartup::isDebugEnabled();
}

void CommandsSunBoxDevtoolsInterface::handleCommand(const String& cmd) {
    String command = cmd;
    command.trim();
    if (command.length() == 0) return;
    
    // Parse command and arguments
    int spaceIndex = command.indexOf(' ');
    String baseCommand = (spaceIndex > 0) ? command.substring(0, spaceIndex) : command;
    String args = (spaceIndex > 0) ? command.substring(spaceIndex + 1) : "";
    
    // Handle commands
    if (baseCommand == "help" || baseCommand == "?") {
        handleHelp();
    }
    else if (baseCommand == "status") {
        handleStatus();
    }
    else if (baseCommand == "debug") {
        handleDebug();
    }
    else if (baseCommand == "dump") {
        handleDump();
    }
    else if (baseCommand == "claimcorrection") {
        handleClaimCorrection(args);
    }
    else if (baseCommand == "claimclear") {
        handleClaimClear();
    }
    else if (baseCommand == "vendortest") {
        handleVendorTest();
    }
    else if (baseCommand == "vendorsend") {
        handleVendorSend();
    }
    else if (baseCommand == "monitorvendor") {
        handleMonitorVendor();
    }
    else {
        Serial4.print("S: Unknown command: ");
        Serial4.println(baseCommand);
        Serial4.println("S: Type 'help' for available commands");
    }
}

void CommandsSunBoxDevtoolsInterface::handleHelp() {
    Serial4.println("\nI: === SunBox DevTools Commands ===");
    Serial4.println("I: help           - Show this help");
    Serial4.println("I: status         - Show system status");
    Serial4.println("I: debug          - Toggle debug mode (persistent)");
    Serial4.println("I: dump           - Dump USB device descriptors");
    Serial4.println("I: claimcorrection vid,pid,interface,endpoint - Force specific interface");
    Serial4.println("I:                Example: claimcorrection 046d,c53f,1,82");
    Serial4.println("I: claimclear     - Clear forced interface configuration");
    Serial4.println("I: vendortest     - Test vendor control transfers (Pwnage mouse)");
    Serial4.println("I: vendorsend     - Send single vendor SET_REPORT");
    Serial4.println("I: monitorvendor  - Toggle vendor transfer monitoring");
    Serial4.println("I: =================================");
}

void CommandsSunBoxDevtoolsInterface::handleStatus() {
    Serial4.println("\nI: === System Status ===");
    
    // USB Device status
    Serial4.print("I: USB Device: ");
    if (usbHostDriver && usbHostDriver->isReady()) {
        Serial4.print("Connected (VID:0x");
        Serial4.print(usbHostDriver->getVendorID(), HEX);
        Serial4.print(" PID:0x");
        Serial4.print(usbHostDriver->getProductID(), HEX);
        Serial4.println(")");
    } else {
        Serial4.println("Not connected");
    }
    
    // HID Handler status
    Serial4.print("I: HID Handler: ");
    if (hidHandler && hidHandler->isReady()) {
        Serial4.print("Ready (Interface ");
        Serial4.print(hidHandler->getInterfaceNumber());
        Serial4.print(", EP 0x");
        Serial4.print(hidHandler->getEndpointAddress() | 0x80, HEX);
        Serial4.println(")");
    } else {
        Serial4.println("Not ready");
    }
    
    // Debug mode
    Serial4.print("I: Debug Mode: ");
    Serial4.print(debugEnabled ? "ON" : "OFF");
    Serial4.println(" (persistent)");
    
    // Vendor monitoring
    Serial4.print("I: Vendor Monitor: ");
    Serial4.print(vendorMonitorEnabled ? "ON" : "OFF");
    if (vendorTransferCount > 0) {
        Serial4.print(" (");
        Serial4.print(vendorTransferCount);
        Serial4.print(" transfers)");
    }
    Serial4.println();
    
    // Force claim config
    ClaimConfig config;
    if (sunboxEEPROM.loadClaimConfig(config)) {
        Serial4.print("I: Claim Correction: VID=0x");
        Serial4.print(config.vid, HEX);
        Serial4.print(" PID=0x");
        Serial4.print(config.pid, HEX);
        Serial4.print(" Interface=");
        Serial4.print(config.interface_num);
        Serial4.print(" Endpoint=0x");
        Serial4.println(config.endpoint_addr, HEX);
    } else {
        Serial4.println("I: Claim Correction: Not configured");
    }
    
    Serial4.println("I: ====================");
}

void CommandsSunBoxDevtoolsInterface::handleDebug() {
    // Toggle debug mode using EEPROM
    debugEnabled = sunboxEEPROM.toggleDebugMode();
    
    // Update the global debug state in SunBoxStartup
    SunBoxStartup::setDebugEnabled(debugEnabled);
    
    // Update debug mode in components if they exist
    if (hidHandler) {
        hidHandler->setDebugOutput(debugEnabled);
    }
    
    Serial4.print("S: Debug mode ");
    Serial4.print(debugEnabled ? "ON" : "OFF");
    Serial4.println(" (saved to EEPROM)");
}

void CommandsSunBoxDevtoolsInterface::handleDump() {
    if (usbHostDriver) {
        usbHostDriver->dumpDeviceInfo();
    } else {
        Serial4.println("S: USB Host Driver not available");
    }
}

void CommandsSunBoxDevtoolsInterface::handleClaimCorrection(const String& args) {
    // Parse format: vid,pid,interface,endpoint
    int commas[3];
    int commaCount = 0;
    
    for (unsigned int i = 0; i < args.length() && commaCount < 3; i++) {
        if (args.charAt(i) == ',') {
            commas[commaCount++] = i;
        }
    }
    
    if (commaCount != 3) {
        Serial4.println("S: Invalid format! Use: claimcorrection vid,pid,interface,endpoint");
        Serial4.println("S: Example: claimcorrection 046d,c53f,1,82");
        return;
    }
    
    // Parse values
    String vidStr = args.substring(0, commas[0]);
    String pidStr = args.substring(commas[0] + 1, commas[1]);
    String ifaceStr = args.substring(commas[1] + 1, commas[2]);
    String epStr = args.substring(commas[2] + 1);
    
    // Convert to numbers
    uint16_t vid = strtoul(vidStr.c_str(), NULL, 16);
    uint16_t pid = strtoul(pidStr.c_str(), NULL, 16);
    uint8_t iface = strtoul(ifaceStr.c_str(), NULL, 10);
    uint8_t ep = strtoul(epStr.c_str(), NULL, 16);
    
    // Save to EEPROM
    if (sunboxEEPROM.saveClaimConfig(vid, pid, iface, ep)) {
        Serial4.println("S: Claim correction configuration saved:");
        Serial4.print("S: VID=0x");
        Serial4.print(vid, HEX);
        Serial4.print(" PID=0x");
        Serial4.print(pid, HEX);
        Serial4.print(" Interface=");
        Serial4.print(iface);
        Serial4.print(" Endpoint=0x");
        Serial4.println(ep, HEX);
        Serial4.println("S: Configuration will be used on next device connection");
    } else {
        Serial4.println("S: Failed to save configuration");
    }
}

void CommandsSunBoxDevtoolsInterface::handleClaimClear() {
    sunboxEEPROM.clearClaimConfig();
    Serial4.println("S: Claim correction configuration cleared");
}

void CommandsSunBoxDevtoolsInterface::handleVendorTest() {
    if (!usbHostDriver || !usbHostDriver->isReady()) {
        Serial4.println("E: No device connected!");
        return;
    }
    
    Serial4.println("\nS: === Vendor Control Transfer Test ===");
    Serial4.println("S: Testing Pwnage mouse vendor commands...");
    
    // Test data from the logs - this is what the software sends
    uint8_t testData[] = {
        0x00, 0x00, 0x02, 0x06, 0x00, 0x81, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // First, let's send a SET_REPORT through the host driver
    Serial4.println("\nS: Test 1: Sending SET_REPORT via host driver...");
    
    // Pause any data transfers
    usbHostDriver->pauseDataTransfers();
    
    // Try the control transfer
    uint16_t actualLen = 0;
    bool success = usbHostDriver->controlTransfer(
        0x21,   // bmRequestType: Host-to-Device, Class, Interface
        0x09,   // bRequest: SET_REPORT
        0x0300, // wValue: Report Type (3=Feature) and Report ID (0)
        0x0002, // wIndex: Interface 2
        64,     // wLength: 64 bytes
        testData,
        &actualLen,
        1000    // 1 second timeout
    );
    
    if (success) {
        Serial4.print("S: SET_REPORT successful! Device accepted ");
        Serial4.print(actualLen);
        Serial4.println(" bytes");
    } else {
        Serial4.println("E: SET_REPORT failed!");
    }
    
    // Now try a GET_REPORT to see what the device responds with
    Serial4.println("\nS: Test 2: Sending GET_REPORT via host driver...");
    
    uint8_t responseBuffer[64];
    memset(responseBuffer, 0, sizeof(responseBuffer));
    
    success = usbHostDriver->controlTransfer(
        0xA1,   // bmRequestType: Device-to-Host, Class, Interface
        0x01,   // bRequest: GET_REPORT
        0x0300, // wValue: Report Type (3=Feature) and Report ID (0)
        0x0002, // wIndex: Interface 2
        64,     // wLength: 64 bytes
        responseBuffer,
        &actualLen,
        1000    // 1 second timeout
    );
    
    if (success && actualLen > 0) {
        Serial4.print("S: GET_REPORT successful! Received ");
        Serial4.print(actualLen);
        Serial4.println(" bytes:");
        
        Serial4.print("S: Response: ");
        for (uint16_t i = 0; i < actualLen && i < 16; i++) {
            if (responseBuffer[i] < 0x10) Serial4.print("0");
            Serial4.print(responseBuffer[i], HEX);
            Serial4.print(" ");
        }
        if (actualLen > 16) Serial4.print("...");
        Serial4.println();
    } else {
        Serial4.println("E: GET_REPORT failed or returned no data!");
    }
    
    // Resume data transfers
    usbHostDriver->resumeDataTransfers();
    
    Serial4.println("\nS: === Vendor Test Complete ===");
}

void CommandsSunBoxDevtoolsInterface::handleVendorSend() {
    if (!usbHostDriver || !usbHostDriver->isReady()) {
        Serial4.println("E: No device connected!");
        return;
    }
    
    Serial4.println("\nS: Sending single vendor SET_REPORT...");
    
    // Simple test data
    uint8_t testData[64] = {0};
    testData[0] = 0x00;
    testData[1] = 0x00;
    testData[2] = 0x02;
    testData[3] = 0x01;
    testData[4] = 0x00;
    testData[5] = 0x85;
    
    // Pause data transfers
    usbHostDriver->pauseDataTransfers();
    
    uint16_t actualLen = 0;
    bool success = usbHostDriver->controlTransfer(
        0x21,   // bmRequestType
        0x09,   // SET_REPORT
        0x0300, // wValue
        0x0002, // Interface 2
        64,     // wLength
        testData,
        &actualLen,
        500
    );
    
    Serial4.print("S: Result: ");
    Serial4.println(success ? "SUCCESS" : "FAILED");
    
    // Resume data transfers
    usbHostDriver->resumeDataTransfers();
}

void CommandsSunBoxDevtoolsInterface::handleMonitorVendor() {
    vendorMonitorEnabled = !vendorMonitorEnabled;
    
    Serial4.print("S: Vendor transfer monitoring ");
    Serial4.println(vendorMonitorEnabled ? "ENABLED" : "DISABLED");
    
    if (!vendorMonitorEnabled) {
        Serial4.print("S: Total transfers monitored: ");
        Serial4.println(vendorTransferCount);
        vendorTransferCount = 0;
    }
}