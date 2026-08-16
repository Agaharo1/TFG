

$ErrorActionPreference = "Stop"

$dbName = "sensores"
$containerName = "influxdb"

Write-Host "Borrando base de datos '$dbName'..."
docker exec $containerName influx -execute "DROP DATABASE $dbName"
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR al borrar la base de datos. Revisa que el contenedor '$containerName' este corriendo (docker ps)."
    exit 1
}

Write-Host "Recreando base de datos '$dbName'..."
docker exec $containerName influx -execute "CREATE DATABASE $dbName"
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR al recrear la base de datos."
    exit 1
}

Write-Host "Listo. Base de datos '$dbName' vacia."
