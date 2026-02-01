# Ciallo～(∠・ω< )⌒★

# **使用说明**

### **准备工作**

首先，你需要安装C++的编译器，并将目录添加到PATH路径中。具体的方法可以自己找教程，这里不细说了。

接着，你需要确保已经安装过python。你可以通过在cmd或者powershell中输入 `python -V` 来检查。

### **开始使用**

你可以在[这里]([Release v1.0 · zsb00000/Code-Checker](https://github.com/zsb00000/Code-Checker/releases/tag/v1.0))下载release。下载后，解压到你需要的文件夹里。  

在你解压后的文件夹里启动powershell。

先安装Flask库：

```bash
cd ./src/frontend
pip install flask
```

然后输入：

```bash
python app.py
```

项目就会开始运行了。你可以打开浏览器，输入 `127.0.0.1:5000` 即可访问前端界面。  

界面如图：![](https://cdn.luogu.com.cn/upload/image_hosting/r8bveb02.png)

你需要准备好数据生成文件、暴力文件、待验证文件，分别上传。  

其中数据生成文件不可以有输入，所有文件不需要文件输入输出。  

你可以自行选择语言标准，并设置时间和空间限制。（但为了防止出问题，时间应小于10s，空间应小于1GB）

测试结束后会返回测试结果，如图：

对于正常测试完成的非AC结果，可以选择下载数据文件和日志文件。界面长这样：![](https://cdn.luogu.com.cn/upload/image_hosting/83iam065.png)

就是这样子了喵~~