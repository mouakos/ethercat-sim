"""
Exemple de scénario — simulation d'une surchauffe.

C'est ici qu'on «joue» les events dans le temps pour tester
le programme PLC : est-ce qu'il réagit correctement ?

Usage :
    python scenario_example.py
"""

import asyncio
import time
from el3001_analog_input import El3001Device, SignalMode
from iolink_temp_sensor import IOLinkTempSensor


async def scenario_surchauffe(sensor: IOLinkTempSensor) -> None:
    """
    Simule une montée en température progressive jusqu'à l'alarme.

    Le PLC doit :
      1. Lire la température (via EL6224 PDO)
      2. Déclencher une alarme quand > 80°C
      3. Couper la sortie moteur (EL2008 DO)
    """
    print("[scénario] démarrage — température normale")
    sensor.temperature_celsius = 22.0
    await asyncio.sleep(5)

    print("[scénario] montée en température (simulation d'un moteur qui chauffe)")
    for step in range(70):
        sensor.temperature_celsius = 22.0 + step * 1.0  # +1°C par seconde
        if step % 10 == 0:
            print(f"[scénario] température = {sensor.temperature_celsius:.1f} °C "
                  f"{'⚠ ALARME' if sensor.alarm_active else ''}")
        await asyncio.sleep(1)

    print("[scénario] fin — température atteinte :", sensor.temperature_celsius, "°C")


async def scenario_panne_capteur(analog: El3001Device) -> None:
    """
    Simule un capteur analogique qui tombe en panne (valeur figée à 0).

    Le PLC doit détecter que la valeur ne bouge plus et lever un défaut.
    """
    print("[scénario] capteur analogique normal (sinus)")
    analog.mode = SignalMode.SINE
    await asyncio.sleep(10)

    print("[scénario] panne capteur — valeur figée à 0")
    analog.mode  = SignalMode.FIXED
    analog.fixed_value = 0.0
    await asyncio.sleep(10)

    print("[scénario] rétablissement")
    analog.mode = SignalMode.SINE


async def main() -> None:
    analog = El3001Device(mode=SignalMode.SINE, amplitude=20000.0, period_s=6.0)
    sensor = IOLinkTempSensor(alarm_high=80.0)

    # montrer quelques ticks de process data
    print("=== Process data EL3001 ===")
    for _ in range(5):
        status, value = analog.tick()
        print(f"  status=0x{status:04x}  ai_value={value:6d}  ({value * 0.3:.1f} mV)")
        await asyncio.sleep(0.05)

    print("\n=== Process data capteur IO-Link ===")
    sensor.temperature_celsius = 22.5
    raw = sensor.process_data()
    print(f"  bytes={raw.hex()}  = {int.from_bytes(raw, 'little', signed=True) * 0.1:.1f} C")

    print("\n=== Scénario surchauffe (accéléré) ===")
    # version accélérée pour la démo
    sensor.alarm_high = 30.0
    for t in range(15):
        sensor.temperature_celsius = 22.0 + t * 1.5
        alarm = "! ALARME" if sensor.alarm_active else "OK"
        print(f"  t={t:2d}s  {sensor.temperature_celsius:.1f}C  {alarm}")
        await asyncio.sleep(0.2)

    print("\n=== Lecture/écriture ISDU ===")
    print("  lire index 0x0018 (nom) :", sensor.isdu_read(0x0018))
    print("  lire index 0x0020 (valeur) :", sensor.isdu_read(0x0020))
    import struct
    sensor.isdu_write(0x0082, struct.pack("<f", 50.0))   # changer alarme haute
    print("  alarme haute après écriture ISDU :", sensor.alarm_high, "°C")

    print("\n=== Lecture/écriture CoE EL3001 ===")
    print("  lire 0x8000:06 (filtre) :", analog.od_read(0x8000, 0x06))
    analog.od_write(0x8000, 0x11, 500)  # offset +500 counts
    status, value = analog.tick()
    print(f"  après offset +500 : ai_value={value}")


if __name__ == "__main__":
    asyncio.run(main())
