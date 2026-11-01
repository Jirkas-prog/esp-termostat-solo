"""Editable SVG, net-labelled wiring overview for the six existing signals."""
from pathlib import Path
from html import escape

root=Path(__file__).resolve().parents[1]
s=['<svg xmlns="http://www.w3.org/2000/svg" width="1320" height="1460" viewBox="0 0 1320 1460">',
   '<rect width="1320" height="1460" fill="#f4f7fb"/>',
   '<style>text{font-family:Arial,sans-serif;fill:#152b42} .small{font-size:16px}</style>']
def text(x,y,t,size=18,color='#152b42',bold=False):
    s.append(f'<text x="{x}" y="{y}" font-size="{size}" style="fill:{color}" font-weight="{700 if bold else 400}">{escape(t)}</text>')
def box(x,y,w,h,fill='#fff',stroke='#71879c',dash=False):
    s.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="12" fill="{fill}" stroke="{stroke}" stroke-width="2"'+(' stroke-dasharray="8 6"' if dash else '')+'/>')
def wire(points,color='#167896',dash=False):
    s.append(f'<polyline points="{points}" fill="none" stroke="{color}" stroke-width="3"'+(' stroke-dasharray="7 5"' if dash else '')+'/>')
def dot(x,y,color='#167896'):
    s.append(f'<circle cx="{x}" cy="{y}" r="5" fill="{color}"/>')

text(40,52,'ESP TERMOSTAT SOLO',32,bold=True)
text(40,85,'T-Display • dvě sondy na 3,3 V • Wi-Fi / MQTT • upravený konektor',22)
box(40,103,1240,42,'#e1f3e7','#64a77b')
text(60,131,'FÁZE 1: režim NONE — měření a simulace, budič ani relé zatím nepřipojovat.',19,bold=True)

box(40,177,340,360)
text(64,212,'LILYGO / TTGO T-Display',23,bold=True)
text(64,240,'Původní ESP32, ne S3',18)
box(65,260,285,120,'#112b42','#112b42')
text(80,289,'Vstup       Výstup',21,'#ffffff',True)
text(80,316,'Průměr / Min / Max / ΔT',18,'#d6efff')
text(80,343,'ON / OFF — SIMULACE',19,'#ffdf7d',True)
text(65,411,'GPIO27 / GPIO26: 1-Wire',18)
text(65,441,'GPIO25 / GPIO33: IN1 / IN2',18)
text(65,480,'5 V vstup přes USB / 5V pin',18,bold=True)
text(65,508,'3V3 regulátor → napájení sond',17)

box(450,177,200,360,'#eaf0f6')
text(471,208,'KONEKTOR',20,bold=True)
pins=[('GND',245,'#4e6070'),('3V3',291,'#be4536'),('GPIO27',337,'#167896'),('GPIO26',383,'#167896'),('GPIO25',429,'#9064a8'),('GPIO33',475,'#9064a8')]
for label,y,c in pins:
    text(476,y+6,label,20,c,True);dot(635,y,c)
text(455,564,'5V vodič přepojit na 3V3!',16,'#a54822',True)
text(455,585,'Pořadí ověř podle pájení.',16)
wire('380,390 415,390 415,354 450,354','#71879c')
wire('650,245 770,245','#4e6070');text(680,234,'GND společná',16)
wire('650,291 770,291','#be4536');text(681,279,'3V3 → VDD',16,'#be4536')

box(850,177,430,160)
text(870,210,'SONDA 1 — VSTUP',23,bold=True)
text(870,240,'DS18B20 před výměníkem',19)
text(870,268,'VDD = 3,3 V    GND = společná',18)
text(870,304,'DQ → GPIO27, HIGH = 3,3 V',18,'#167896',True)
wire('650,337 800,337 800,299 850,299')

box(850,365,430,160)
text(870,398,'SONDA 2 — VÝSTUP',23,bold=True)
text(870,428,'DS18B20 za výměníkem',19)
text(870,456,'VDD = 3,3 V    GND = společná',18)
text(870,492,'DQ → GPIO26, HIGH = 3,3 V',18,'#167896',True)
wire('650,383 780,383 780,487 850,487')

box(850,565,430,208,'#f5f0fa','#9064a8',True)
text(870,597,'POZDĚJI: DRV8833 + 5V relé',22,bold=True)
text(870,628,'AIN1 = GPIO25   AIN2 = GPIO33',18)
text(870,658,'VM = 5 V samostatně; ne z 3V3 konektoru',17)
text(870,688,'nSLEEP → 47k → 5 V; vstupy → 10k → GND',16)
text(870,718,'Cívka mezi AOUT1 a AOUT2',18)
text(870,749,'00 = bez buzení • nyní nepřipojovat',18,'#75478e',True)
wire('650,429 720,429 720,617 850,617','#9064a8',True)
wire('650,475 700,475 700,641 850,641','#9064a8',True)

box(40,620,620,300,'#fff4dd','#d3a94a')
text(62,652,'DQ PULL-UP: externí odpory uvnitř pouzdra',21,bold=True)
text(62,679,'Skutečný pin 3V3 — ne GPIO nastavený HIGH',19,'#a54822',True)
wire('180,706 530,706','#a54822')
for x,pin in [(230,'27'),(500,'26')]:
    dot(x,706,'#a54822');wire(f'{x},706 {x},728','#a54822')
    s.append(f'<rect x="{x-8}" y="728" width="16" height="42" fill="#fff" stroke="#a54822" stroke-width="2"/>')
    text(x+17,754,'4,7 kΩ',16)
    wire(f'{x},770 {x},801');dot(x,801)
    text(x-55,830,'DQ / GPIO'+pin,17)
text(62,865,'Interní pull-up GPIO27 i GPIO26 je zapnutý (0.2.0).',18,'#a54822',True)
text(62,893,'Sondy přímo, bez adaptérů; každá vlastní 4,7kΩ.',17)

box(700,805,580,115)
text(721,836,'TTP224 (volitelně), napájení 3,3 V',21,bold=True)
text(721,867,'Zámek 32  /  − 17  /  + 36  /  Uložit 39',20)
text(721,896,'Momentové HIGH; každý vstup pull-down 10k na GND.',16)

box(40,950,1240,262)
text(62,983,'PRINCIP ZÁLOHY PO PŘIDÁNÍ RELÉ',22,bold=True)
text(62,1012,'Elektronický a mechanický kontakt musí čerpadlo spustit každý samostatně.',19)
wire('90,1095 280,1095 280,1050 430,1050','#be4536')
wire('280,1095 280,1150 430,1150','#be4536');dot(280,1095,'#be4536')
box(430,1030,350,40,'#eaf0f6');text(447,1057,'K1: kontakt relé (např. COM–NC)',17)
box(430,1130,350,40,'#fff4dd');text(447,1157,'TH: mechanický spínací termostat',17)
wire('780,1050 860,1050 860,1095 1020,1095','#be4536')
wire('780,1150 860,1150 860,1095','#be4536');dot(860,1095,'#be4536')
text(70,1078,'Stejný jištěný přívod',16)
box(1020,1064,225,64,'#e1f3e7');text(1037,1103,'ČERPADLO',23,bold=True)
text(70,1193,'Pouze funkční princip: dimenzování kontaktů, N/PE a ochrany řeší skutečný silový projekt.',16)

box(40,1240,1240,168,'#eaf0f6')
text(62,1272,'Chování regulátoru',22,bold=True)
text(62,1302,'Zdroj ZAP a zdroj VYP nezávisle: vstup / výstup / průměr / minimum / maximum.',19)
text(62,1330,'ΔT = výstup − vstup. Neplatné čidlo = null + požadavek ON. ON má přednost před OFF.',18)
text(62,1358,'NONE nebudí cívku; neznamená rozepnutý NC kontakt. Mechanická záloha musí být automatická.',18)
text(62,1386,'PE014F12 má 12V cívku: nepatří na 5 V ani na DRV8833 napájený 12 V.',18,'#a54822',True)
text(40,1441,'SOLO 0.2.0 • 30. 10. 2026 • Schéma pro ověření, nikoliv výrobní silový podklad. Podrobnosti: ZAPOJENI.md',16)
s.append('</svg>')
(root/'docs/ZAPOJENI.svg').write_text('\n'.join(s),encoding='utf-8')
print('Wrote docs/ZAPOJENI.svg')
