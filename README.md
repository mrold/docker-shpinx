## Docker 支持中文分词的Sphinx搜索引擎 
### 基于coreseek-3.2.14改造(部分c语法老旧) 

####目录结构

```
docker-sphinx
|-- coreseek-3.2.14               coreseek源码包 
|-- etc     
    |-- csft.conf                 sphinx配置文件
    |-- example.sql               案例sql文件
    |-- sphinx-min.conf.dist      简单配置参考
    |-- sphinx.conf.dist          复杂配置参考
|-- Dockerfile
|-- entrypint.sh
|-- README.md
```
#### 使用步骤

1. 正确配置csft.conf文件。如果需要测试。创建数据库 > 执行example.sql
2. buid镜像 `docker build -t my-sphinx:v1 .`
3. 启动一个新容器`docker run ...`

#### 其它