# Push demo for Crazyflie 2.x

This repos contains the push demo for Cazyflie 2.x.
It uses out-of-tree build functionality of the Crazyflie firmware and is implemented using the app entry-point.

This demo works with a Crazyflie 2.x with Flow deck version 2.0 and Multiranger deck attached. **BOTH THESE ATTACHEMENTS ARE REQUIRED FOR THE DEMO TO RUN.** If you want to use the Flow deck 1.0 or any other positioning deck, you will need to change the following line to look at the parameter for the wanted deck:
```
  uint16_t idPositioningDeck = paramGetVarId("deck", "bcFlow2");

```

When running, the crazyflie will wait for an object to get close to the top ranging sensor.
When an oject (ie. an hand) has passed close to the top sensor, the Crazyflie takes off and hover.
If anything is detected on side sensors, the Crazyflie moves in the oposite direction.
So, it is possible to push the Crazyflie around.

Per default, the Crazyflie will fly on 0.2m. If something is detected closer than 10cm on both sides it will go up, if front and back (or 30cm from the top) it will go down. It will land once it is closer than 10cm to the ground.

## Build

You must have the required tools to build the [Crazyflie firmware](https://github.com/bitcraze/crazyflie-firmware).

Clone the repos with ```--recursive```. If you did not do so, pull submodules with:
```
git submodule update --init --recursive
```

Then build and bootload:
```
make -j$(nproc)
make cload
```

## Running

Once the app layer is bootloaded, the demo will automatically be running at idle state on the crazyflie. To start the demo, place your hand ontop of the drone (within 1 inch of the multiranger) for 3 seconds, and quickly remove it to prevent collision during take off 

