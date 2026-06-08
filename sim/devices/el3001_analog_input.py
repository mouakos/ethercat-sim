"""
EL3001 — Modèle d'appareil : entrée analogique 1 canal.

Ce fichier est une ESQUISSE de ce que sera la couche L4.
Il n'est pas encore branché sur le stack C++ (L3).

Rôle : fournir une valeur analogique simulée à l'esclave EtherCAT.
Le stack C++ (chain.cpp) appelle tick() toutes les ~50 ms pour
récupérer la valeur à mettre dans le PDO (process data object).
"""

import math
import time
from dataclasses import dataclass, field
from enum import Enum


class SignalMode(Enum):
    SINE  = "sine"   # sinus — simule un capteur qui oscille
    RAMP  = "ramp"   # rampe — monte puis redescend
    FIXED = "fixed"  # valeur fixe — simule un capteur immobile


@dataclass
class El3001Device:
    """
    Modèle du terminal EL3001 (entrée analogique 12 bits).

    Process data (ce que TwinCAT lit en cyclique via LRD) :
      - status_word : mot d'état (bit 15 = TxPDO Toggle, doit alterner)
      - ai_value    : valeur INT16, unités = ~0.3 mV/count pour ±10 V

    CoE Object Dictionary (ce que TwinCAT lit/écrit en acyclique via SDO) :
      - 0x8000:06  filter_enabled     — active le filtre passe-bas
      - 0x8000:11  user_scale_offset  — décalage utilisateur (INT16)
    """

    # --- comportement du signal ---
    mode: SignalMode = SignalMode.SINE
    amplitude: float = 20000.0   # ±20000 counts ≈ ±6 V
    period_s: float  = 6.0       # période de la sinus en secondes
    fixed_value: float = 0.0     # utilisé si mode = FIXED

    # --- paramètres CoE (0x8000) — modifiables par TwinCAT ---
    filter_enabled: bool = False
    user_scale_offset: int = 0   # INT16 : décalage ajouté à la valeur brute

    # --- état interne ---
    _start: float = field(default_factory=time.monotonic, init=False)
    _toggle: bool = field(default=False, init=False)

    # ------------------------------------------------------------------
    # Interface appelée par le stack C++ (L3) toutes les ~50 ms
    # ------------------------------------------------------------------

    def tick(self) -> tuple[int, int]:
        """
        Calcule la valeur courante du capteur.

        Retourne (status_word, ai_value) prêts à écrire dans le PDO.
        Le stack C++ les copie dans les registres de l'ESC simulé.
        """
        t = time.monotonic() - self._start

        match self.mode:
            case SignalMode.SINE:
                raw = self.amplitude * math.sin(2 * math.pi * t / self.period_s)
            case SignalMode.RAMP:
                phase = (t % self.period_s) / self.period_s  # 0.0 → 1.0
                raw = self.amplitude * (2 * phase - 1)        # -amp → +amp
            case SignalMode.FIXED:
                raw = self.fixed_value

        # appliquer le décalage utilisateur (CoE 0x8000:11)
        value = int(raw) + self.user_scale_offset

        # borner à INT16
        value = max(-32768, min(32767, value))

        # TxPDO Toggle : le bit 15 du status_word doit alterner à chaque
        # mise à jour pour que TwinCAT sache que la donnée est fraîche
        self._toggle = not self._toggle
        status_word = 0x8000 if self._toggle else 0x0000

        return status_word, value

    # ------------------------------------------------------------------
    # Interface CoE — appelée par le stack C++ quand TwinCAT
    # envoie un SDO Upload (lecture) ou Download (écriture)
    # ------------------------------------------------------------------

    def od_read(self, index: int, subindex: int) -> int | None:
        """
        TwinCAT lit un paramètre (SDO Upload).
        Retourne la valeur, ou None si cet objet est géré par le C++.
        """
        match (index, subindex):
            case (0x8000, 0x06):
                return int(self.filter_enabled)
            case (0x8000, 0x11):
                # INT16 retourné comme uint16 sur le fil
                return self.user_scale_offset & 0xFFFF
            case _:
                return None  # le stack C++ gère (0x1000, 0x1018, 0x6000...)

    def od_write(self, index: int, subindex: int, value: int) -> bool:
        """
        TwinCAT écrit un paramètre (SDO Download).
        Retourne True si on l'a traité, False sinon.
        """
        match (index, subindex):
            case (0x8000, 0x06):
                self.filter_enabled = bool(value)
                print(f"[EL3001] filtre {'activé' if self.filter_enabled else 'désactivé'}")
                return True
            case (0x8000, 0x11):
                # interpréter comme INT16 signé
                self.user_scale_offset = value if value < 0x8000 else value - 0x10000
                print(f"[EL3001] offset utilisateur = {self.user_scale_offset}")
                return True
            case _:
                return False
