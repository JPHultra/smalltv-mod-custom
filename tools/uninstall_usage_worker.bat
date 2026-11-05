@echo off
schtasks /Delete /TN SmallTVUsageWorker /F 2>nul
echo Removed SmallTV usage worker scheduled task.
