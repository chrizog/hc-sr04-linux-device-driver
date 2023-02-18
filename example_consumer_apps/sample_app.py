import struct
import time

if __name__ == "__main__":

    while True:
        with open("/dev/hc-sr04", "rb") as file_hc_sr04:
            # Read 4 bytes and convert to an integer value
            range_mm = struct.unpack('i', file_hc_sr04.read(4))[0]
            print("Measured range in mm: " + str(range_mm))

        # Sleep 100 ms
        time.sleep(0.1)