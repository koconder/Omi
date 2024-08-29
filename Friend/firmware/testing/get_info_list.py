import argparse
import os
from dotenv import load_dotenv

import asyncio
import bleak
import numpy as np
from bleak import BleakClient

load_dotenv()

device_id = "3CE1CE0A-A629-2E92-D708-E49E71045D07" #Please enter the id of your device (that is, the device id used to connect to your BT device here)
storage_uuid = "00001542-1212-EFDE-1523-785FEABCD123" #dont change this


async def main():
        async with BleakClient(device_id) as client:

            r = await client.read_gatt_char(storage_uuid)
            await asyncio.sleep(2.0)

            while(True):
                 await asyncio.sleep(2.0)
                 print(r)

        
if __name__ == "__main__":
    asyncio.run(main())


