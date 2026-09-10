# 成本与交期模型

仓库中不存在任何供应商报价。因此该模型将价格与交期留空并报告
`QUOTE_REQUIRED`；在没有带日期报价的情况下填入数字，会造成虚假的商业承诺。

对于每个物料行：

`extended_cost = quantity * quoted_unit_price`

`planning_total = sum(extended_cost) + freight + tax + 10% contingency`

使用关键路径上最长的已确认交期，再加上来料检验与隔离时间。在 AVL Owner 签署
候选 MPN、报价具有有效期且来料验收证据已定义之前，物料不能变为 `ORDERABLE`。

当前 PCB 元件矩阵刻意使用元件类别而非可下单的 MPN。采购必须在签发 PO 之前与
电气 Owner 一起补齐这一缺口。
