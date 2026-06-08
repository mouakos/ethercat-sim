"""
Capteur de température IO-Link — profil générique.

Ce device est branché sur le port 1 de l'EL6224.
Il communique via le protocole IO-Link (couche au-dessus d'EtherCAT) :

  - Process Data (PD)  : valeur de température, envoyée chaque cycle
  - ISDU               : paramètres lus/écrits à la demande
                         (échelle, offset, alarmes, nom...)

L'EL6224 fait le pont entre EtherCAT et IO-Link.
Notre simulateur doit simuler les deux côtés.
"""

from dataclasses import dataclass
import struct


@dataclass
class IOLinkTempSensor:
    """
    Capteur de température IO-Link (profil générique).

    Process Data : 2 octets INT16 = température en 0.1°C
      ex : 225 = 22.5°C

    ISDU (Indexed Service Data Unit) — paramètres accessibles :
      Index 0x0018  Application Specific Name (lecture seule)
      Index 0x0020  Valeur actuelle (lecture seule)
      Index 0x0080  Échelle (écriture)
      Index 0x0081  Offset (écriture)
      Index 0x0082  Seuil alarme haute (écriture)
      Index 0x0083  Seuil alarme basse (écriture)
    """

    temperature_celsius: float = 22.0   # valeur simulée actuelle

    # paramètres configurables par ISDU
    scale: float  = 0.1    # 0.1°C par count
    offset: float = 0.0    # décalage en °C
    alarm_high: float =  80.0   # alarme température trop haute
    alarm_low:  float = -20.0   # alarme température trop basse
    name: str = "SimTempSensor-01"

    # ------------------------------------------------------------------
    # Process Data — appelé chaque cycle EtherCAT (~10 ms)
    # ------------------------------------------------------------------

    def process_data(self) -> bytes:
        """
        Retourne les 2 octets de process data IO-Link.
        L'EL6224 les copie dans son PDO TxPDO pour que TwinCAT les lise.
        """
        counts = int((self.temperature_celsius + self.offset) / self.scale)
        counts = max(-32768, min(32767, counts))  # borner INT16
        return struct.pack("<h", counts)  # little-endian signé

    def port_status(self) -> int:
        """
        État du port IO-Link (0xF100:01 dans l'OD de l'EL6224).
          0x08 = IO-Link en communication OP (normal)
          0xA0 = pas de device
        """
        return 0x08  # on est connecté et en OP

    # ------------------------------------------------------------------
    # ISDU — appelé quand TwinCAT demande un paramètre via CoE/AoE
    # ------------------------------------------------------------------

    def isdu_read(self, index: int) -> bytes | None:
        """
        Lecture d'un paramètre ISDU.
        Retourne les octets de la réponse, ou None si index inconnu.
        """
        match index:
            case 0x0018:
                return self.name.encode("ascii")
            case 0x0020:
                return self.process_data()   # valeur courante
            case 0x0080:
                return struct.pack("<f", self.scale)
            case 0x0081:
                return struct.pack("<f", self.offset)
            case 0x0082:
                return struct.pack("<f", self.alarm_high)
            case 0x0083:
                return struct.pack("<f", self.alarm_low)
            case _:
                return None

    def isdu_write(self, index: int, data: bytes) -> bool:
        """
        Écriture d'un paramètre ISDU.
        Retourne True si traité, False si index inconnu ou lecture seule.
        """
        match index:
            case 0x0080:
                self.scale = struct.unpack("<f", data)[0]
                print(f"[TempSensor] échelle = {self.scale} °C/count")
                return True
            case 0x0081:
                self.offset = struct.unpack("<f", data)[0]
                print(f"[TempSensor] offset = {self.offset} °C")
                return True
            case 0x0082:
                self.alarm_high = struct.unpack("<f", data)[0]
                print(f"[TempSensor] alarme haute = {self.alarm_high} °C")
                return True
            case 0x0083:
                self.alarm_low = struct.unpack("<f", data)[0]
                print(f"[TempSensor] alarme basse = {self.alarm_low} °C")
                return True
            case _:
                return False  # lecture seule ou inconnu

    @property
    def alarm_active(self) -> bool:
        """True si la température dépasse une limite."""
        return (self.temperature_celsius > self.alarm_high or
                self.temperature_celsius < self.alarm_low)
