cmake_minimum_required(VERSION 3.20)
project(vdisk C)
include(VeloCE)

velo_add_library(vdisk vdisk.c EXPORTS VDiskStart DSK_Init DSK_Deinit DSK_Open DSK_Close DSK_Read DSK_Write DSK_Seek DSK_IOControl DSK_PowerUp DSK_PowerDown)
