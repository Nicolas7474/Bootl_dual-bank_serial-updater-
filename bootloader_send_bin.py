import os
import sys
import time
import struct
import zlib  # zlib.crc32 perfectly matches the STM32 Hardware CRC32 (Poly: 0x04C11DB7, Init: 0xFFFFFFFF)
import serial

# Protocol Constants
PACKET_START_BYTE = b'\x02'
ACK_BYTE = b'\x06'
NAK_BYTE = b'\x15'
PAYLOAD_MAX_SIZE = 512


def calc_stm32_crc32(data: bytes) -> int:
    """
    Computes a CRC32 that doesn't ! align with the STM32's hardware CRC unit.
    The STM32 calculates CRC32 over 32-bit words. If data is padded to a 
    multiple of 4 bytes, zlib.crc32 matches the hardware calculation.
    """
    # zlib returns an unsigned 32-bit int, exactly what we need
    return zlib.crc32(data) & 0xFFFFFFFF


def send_firmware(port_name: str, baudrate: int, bin_file_path: str):
    if not os.path.exists(bin_file_path):
        print(f"Error: File not found -> {bin_file_path}")
        return

    # 1. Read binary ('rb') and handle 4-byte padding alignment constraints
    with open(bin_file_path, 'rb') as f:  
        raw_data = f.read()   

    total_size = len(raw_data)
    remainder = total_size % 4
    if remainder != 0:
        padding_needed = 4 - remainder
        raw_data += b'\xFF' * padding_needed
        total_size = len(raw_data)
        print(f"Padded binary with {padding_needed} bytes for 32-bit word alignment.")

    print(f"Opening port {port_name} at {baudrate} baud...")
    print(f"Preparing to send binary file: {bin_file_path} ({total_size} bytes total)")

    try:
        # Open serial port with a timeout to prevent hanging forever on dropped ACKs
        ser = serial.Serial(port=port_name, baudrate=baudrate, timeout=3.0)
    except Exception as e:
        print(f"Failed to open serial port: {e}")
        return

    time.sleep(0.5)  # Let the connection stabilize
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    # 2. Slice file and transmit sequential packets
    bytes_sent = 0
    packet_id = 1
    
    # Generate chunks:
    # list where every element is a bytes object 512 bytes long (except for the last one)
    chunks = [raw_data[i:i + PAYLOAD_MAX_SIZE] for i in range(0, total_size, PAYLOAD_MAX_SIZE)]
    total_packets = len(chunks)

    print("\n--- Starting Flashing Sequence ---")

    for chunk in chunks:
        payload_len = len(chunk)
        
        # Calculate CRC32 for this specific payload block
        payload_crc = calc_stm32_crc32(chunk)

        # Build Header string using Big-Endian packing formats:
        # >IHH -> '>' Big-Endian, 'I' uint32 (4 bytes), 'H' uint16 (2 bytes), 'H' uint16 (2 bytes)
        header_bytes = struct.pack(">IHH", total_size, packet_id, payload_len)
        
        # Build CRC tracking suffix, because you cannot
        # just send or concatenate a raw integer to a bytes object
        crc_bytes = payload_crc.to_bytes(4, 'big')

        # Stitch full frame structure together
        full_packet = PACKET_START_BYTE + header_bytes + chunk + crc_bytes

        # Retry logic loop for robustness against noise
        max_retries = 3
        retry_count = 0
        success = False
   
        while retry_count < max_retries and not success:
            print(f"Sending Packet {packet_id}/{total_packets} (Size: {payload_len} bytes) ... ", end="", flush=True)
         
            ser.reset_input_buffer()    # Flush existing line garbage right before writing
            ser.write(full_packet)
            ser.flush()                 # Ensure system buffers push bytes out to raw copper wires

            # Wait for handshake response byte back from Bootloader
            response = ser.read(1) # 1 byte - with timeout=3.0

            if response == ACK_BYTE:
                print("ACK received!")
                bytes_sent += payload_len
                packet_id += 1
                success = True
            elif response == NAK_BYTE:
                retry_count += 1
                print(f"NAK received! Retrying ({retry_count}/{max_retries})...")
                time.sleep(0.1) # back off for a few ms before to re-sending the same packet
            else:
                retry_count += 1
                print(f"Timeout/No response! Retrying ({retry_count}/{max_retries})...")
                time.sleep(0.2)

        if not success:
            print("\nCritical Error: Failed to receive confirmation from target. Aborting upload.")
            ser.close()
            sys.exit(1)

    print("\n--- Upload Successful! ---")
    print(f"Total processed bytes successfully written: {bytes_sent}/{total_size}")
    
    # If this was the last packet, the bootloader automatically runs:
    # record_new_bank_state() followed by a system lock. 
    # To run your new application, toggle your target board's physical RESET line!
    print("Flashing completed. Power-cycle or hardware-reset your board to boot the new firmware.")
    ser.close()


if __name__ == "__main__":
    # Hardcoded values for quick testing
    com_port = "COM4"
    baud = 115200
    file_path = "application.bin"  # Ensure this file is in the same folder as your script

    print("========================================")
    print(f"Running in hardcoded mode:")
    print(f"Target Port: {com_port}")
    print(f"Baudrate:    {baud}")
    print(f"Binary File: {file_path}")
    print("========================================")

    send_firmware(com_port, baud, file_path)