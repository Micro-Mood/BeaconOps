echo "
====Start-------------------
"
date
echo ""
export my_mcu_target="esp32c3"
export my_flash_freq="80m"
export my_binary_name="BeaconOps"
idf.py -DIDF_TARGET=$my_mcu_target build | grep error:
esptool.py --chip $my_mcu_target merge_bin -o merged-binary.bin -f raw --flash_mode dio --flash_freq $my_flash_freq --flash_size keep 0x0 ./build/bootloader/bootloader.bin 0x10000 ./build/$my_binary_name.bin 0x8000 ./build/partition_table/partition-table.bin
cp "./merged-binary.bin" "/mnt/c/Users/$1/Desktop/$my_binary_name/main.bin"
echo "
====Finish-------------------
"
