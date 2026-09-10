# sim/worlds

每个场景类别的 Gazebo 世界文件（.world / .sdf）。

```
worlds/
  workbench_v0.world     default tabletop, single arm, one camera
  workbench_v0_dual.sdf  dual camera (future)
  inspection_station.sdf flat inspection table, overhead camera (future)
```

世界版本钉在每个场景清单里（`world_version` 字段）。改动世界文件需要提升版本字符串并重新冻结受影响的场景。
