ESP Logger Endpoint (PHP)

Dateien:
- index.php     Router + Auth + Config loader
- push.php      Empfängt JSON (POST) und schreibt in Monats-CSV pro SensorID
- view.php      Chart.js GUI, lädt CSVs per AJAX (on-demand)
- config.php    GUI für API-Keys (nur canEditConfig=true)
- config.json   Server-Konfiguration (Hashes, Rechte, Pfade)
- data/         CSV Ablage (pro Monat + SensorID)
 - data/log.json Server-Log (JSON, 200 Einträge / 30 Tage) (Pfad über config.json: logPath)

Quickstart:
1) Lege config.json an (siehe config.json.example) und schütze sie (z.B. .htaccess).
2) Erstelle einen Admin-Key Hash:
   php -r 'echo password_hash("adminPw", PASSWORD_DEFAULT), PHP_EOL;'
3) Setze in config.json die Hashes und ids (ids können beliebig sein).
4) Für den ESP setze die URL z.B.:
   https://example.com/endpoint/index.php?a=push&sensorID=A1&apikey=12345678
   (ESP sendet dann JSON im Body via POST)

CSV Format:
- Separator ist ';'
- Jede Zeile enthält: ts, ts_str, BME min/max/avg, und DS1..DS16 Werte (sn,t,min,max,avg)
