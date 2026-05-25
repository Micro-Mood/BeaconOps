# Enclosure

这里放的是 BeaconOps 当前用的外壳模型。

## 文件

- `shell.stl`：外壳主体，可直接打印
- `back-cover.stl`：后盖，可直接打印
- `model.3dm`：可编辑的结构源文件
- `model_embedded_files/`：`model.3dm` 关联的嵌入资源

## 为什么和 pocket 是同一套

直说吧：**PCB 是去年就画好的，外壳我懒得再重做一遍，反正和 [pocket](../../../../pocket/hardware) 整个项目尺寸一样，就直接拿来用了**。  
所以这里保留的就是同一套 STL / 3DM，不是有什么深奥的复用设计，就是务实选择。

## 怎么用

- 直接打印，用 STL
- 要改尺寸、开孔、装配细节，先改 `model.3dm`，再导出 STL
- 改之前最好先翻一下 `../PCB/`，确认板尺寸和接口位置对得上

## 后续

如果哪天 BeaconOps 和 pocket 在结构上分叉了，这里就独立维护 BeaconOps 自己的壳体版本，不再硬绑着 pocket。