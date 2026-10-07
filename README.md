# simpleFS
### Сборка
```
make
```

### Создаём образ
```
sudo dd if=/dev/zero of=disk.img bs=512 count=2050
```

### Подключаем loop
```
export DEVICE=$(sudo losetup -f --show disk.img)
echo ${DEVICE}
```

### Загружаем модуль
```
sudo insmod simplefs.ko device_name="$DEVICE" \
    sb_main_sector=0 sb_backup_sector=1024 \
    max_filename_len=64 max_file_sectors=16
```

### Монтируем с форматированием
```
sudo mkdir -p /mnt/simplefs
sudo mount -t simplefs "$DEVICE" /mnt/simplefs -o format
```
### Смотрим файлы
```
ls /mnt/simplefs | head
```
### Прогоняем тесты
```
./simplefs_cli /mnt/simplefs test
./simplefs_cli /mnt/simplefs info
./simplefs_cli /mnt/simplefs metadata
./simplefs_cli /mnt/simplefs map file000
./simplefs_cli /mnt/simplefs zero
./simplefs_cli /mnt/simplefs metadata
./simplefs_cli /mnt/simplefs erase
```

### Завершение работы
```
sudo umount /mnt/simplefs
sudo rmmod simplefs
sudo losetup -d "$DEVICE"
rm disk.img
```
