#include "ops/option.h"
#include "ops/config_manager.h"

#define KEY_TO_STRING(key) (#key)

namespace yuan::net::http::config
{
    namespace
    {
        constexpr int kDefaultConnectionIdleTimeout = 30 * 1000;
        constexpr uint32_t kDefaultMaxHeaderLength = 1024 * 1024;
        constexpr uint32_t kDefaultClientMaxContentLength = 1024 * 1024 * 5;
        constexpr bool kDefaultCloseIdleConnection = true;
        constexpr bool kDefaultFormDataUploadSave = true;
        constexpr bool kDefaultEnableHttp2 = false;
        constexpr bool kDefaultEnableHttp3 = false;

        void reset_runtime_defaults()
        {
            connection_idle_timeout = kDefaultConnectionIdleTimeout;
            max_header_length = kDefaultMaxHeaderLength;
            client_max_content_length = kDefaultClientMaxContentLength;
            close_idle_connection = kDefaultCloseIdleConnection;
            form_data_upload_save = kDefaultFormDataUploadSave;
            enable_http2 = kDefaultEnableHttp2;
            enable_http3 = kDefaultEnableHttp3;
        }
    }

    int connection_idle_timeout = kDefaultConnectionIdleTimeout;
    
    const char * config_file_name = "http.json";

    const char * server_name = KEY_TO_STRING(server_name);

    const char * parse_form_data_content_types = KEY_TO_STRING(parse_form_data_content_types);

    const char * static_file_paths = KEY_TO_STRING(static_file_paths);
    const char * static_file_paths_root = "root";
    const char * static_file_paths_path = "path";

    const char * playable_types = KEY_TO_STRING(playable_types);

    // 最大包体长度默认 2 m
    uint32_t max_header_length = kDefaultMaxHeaderLength;

    uint32_t client_max_content_length = kDefaultClientMaxContentLength;

    bool close_idle_connection = kDefaultCloseIdleConnection;

    bool form_data_upload_save = kDefaultFormDataUploadSave;

    bool enable_http2 = kDefaultEnableHttp2;
    bool enable_http3 = kDefaultEnableHttp3;

    int proxy_connect_timeout = 5 * 1000;

    int proxy_max_pending = 10;

    // 代理最大缓冲区
    int proxy_buffer_max = 1024 * 1024 * 3;

    void load_config()
    {
        reset_runtime_defaults();

        auto cfgManager = HttpConfigManager::get_instance();
        if (!cfgManager->good()) {
            return;
        }

        connection_idle_timeout = cfgManager->get_uint_property(KEY_TO_STRING(connection_idle_timeout), connection_idle_timeout);
        client_max_content_length = cfgManager->get_uint_property(KEY_TO_STRING(max_content_length), client_max_content_length);
        max_header_length = cfgManager->get_uint_property(KEY_TO_STRING(max_header_length), max_header_length);
        close_idle_connection = cfgManager->get_bool_property(KEY_TO_STRING(close_idle_connection), close_idle_connection);
        form_data_upload_save = cfgManager->get_bool_property(KEY_TO_STRING(form_data_upload_save), form_data_upload_save);
        enable_http2 = cfgManager->get_bool_property(KEY_TO_STRING(enable_http2), enable_http2);
        enable_http3 = cfgManager->get_bool_property(KEY_TO_STRING(enable_http3), enable_http3);
    }

    const std::string_view file_list_html_text = 
R"(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>文件列表</title>
    <style>
        body {
            font-family: 'Arial', sans-serif;
            line-height: 1.6;
            margin: 0;
            padding: 20px;
            background-color: #f5f5f5;
        }
        .container {
            max-width: 800px;
            margin: 0 auto;
            background: white;
            padding: 20px;
            border-radius: 8px;
            box-shadow: 0 2px 10px rgba(0,0,0,0.1);
        }
        h1 {
            color: #333;
            border-bottom: 1px solid #eee;
            padding-bottom: 10px;
        }
        .file-list {
            list-style: none;
            padding: 0;
        }
        .file-item {
            display: flex;
            align-items: center;
            padding: 10px;
            border-bottom: 1px solid #eee;
            transition: background-color 0.2s;
        }
        .file-item:hover {
            background-color: #f9f9f9;
        }
        .file-icon {
            margin-right: 15px;
            color: #555;
            width: 24px;
            text-align: center;
        }
        .file-name {
            flex-grow: 1;
            color: #333;
            cursor: pointer;
            text-decoration: none;
        }
        .file-name:hover {
            color: #0066cc;
            text-decoration: underline;
        }
        .file-info {
            display: flexbox;
            align-items: center;
            color: #777;
            font-size: 0.9em;
            margin: 0 auto;
        }
        .file-size {
            margin-right: 15px;
            min-width: 80px;
        }
        .file-date {
            min-width: 100px;
        }
        .download-btn {
            background: none;
            border: none;
            color: #0066cc;
            cursor: pointer;
            font-size: 1.2em;
            padding: 0 5px;
            margin-left: 10px;
        }
        .download-btn:hover {
            color: #004499;
        }
        .loading, .error {
            color: #777;
            text-align: center;
            padding: 20px;
        }
        .error {
            color: #d9534f;
        }
        .refresh-btn {
            background-color: #5bc0de;
            color: white;
            border: none;
            padding: 8px 15px;
            border-radius: 4px;
            cursor: pointer;
            margin-top: 15px;
        }
        .refresh-btn:hover {
            background-color: #46b8da;
        }
        .directory .file-icon {
            color: #f0ad4e;
        }
        .directory .file-name {
            font-weight: bold;
        }
        .directory .file-size {
            color: #999;
        }
        .directory .file-date {
            color: #999;
        }
        .back-btn {
            background-color: #d9534f;
            color: white;
            border: none;
            padding: 8px 15px;
            border-radius: 4px;
            cursor: pointer;
            margin-top: 15px;
            float: right;
        }
        .back-btn:hover {
            background-color: #d43f3a;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>文件列表(<span id="dirName"></span> - <span id="fileCount"></span>个文件)</h1>
        
        <div id="fileListContainer">
            <div class="loading">正在加载文件列表...</div>
        </div>
        
        <div>
            <button id="refreshBtn" class="refresh-btn">刷新列表</button>
            <button id="backBtn" class="back-btn" style="display: none;">返回上级目录</button>
        </div>
    </div>

    <script>
        // 文件列表API端点 - 替换为你的实际API地址
        const apiPrefix = window.location.href.substring(0, window.location.href.lastIndexOf('/'));
        var apiUrl = apiPrefix + '/';
        var basePath = apiPrefix.substring(apiPrefix.lastIndexOf('/'))
        // 或者使用相对路径 '/api/files'
        var processing = false; // 防止重复请求
        
        // 获取文件列表
        async function fetchFileList() {
            const container = document.getElementById('fileListContainer');
            container.innerHTML = '<div class="loading">正在加载文件列表...</div>';
            
            try {
                console.log('请求文件列表:', apiUrl);
                const response = await fetch(apiUrl);
                
                if (!response.ok) {
                    throw new Error(`HTTP错误! 状态码: ${response.status}`);
                }
                
                const files = await response.json();
                
                if (!Array.isArray(files)) {
                    throw new Error('返回的数据不是有效的文件列表');
                }
                
                renderFileList(files);
            } catch (error) {
                console.error('获取文件列表失败:', error);
                container.innerHTML = `
                    <div class="error">
                        加载文件列表失败: ${error.message}
                        <br>
                        <small>${new Date().toLocaleString()}</small>
                    </div>
                `;
            }
        }
        
        // 获取文件图标
        function getFileIcon(filename, type) {
            const ext = filename.split('.').pop().toLowerCase();
            const icons = {
                pdf: "📄",
                doc: "📝", docx: "📝",
                xls: "📊", xlsx: "📊", csv: "📊",
                jpg: "🖼️", jpeg: "🖼️", png: "🖼️", gif: "🖼️", svg: "🖼️",
                ppt: "📑", pptx: "📑",
                zip: "🗜️", rar: "🗜️", '7z': "🗜️", tar: "🗜️", gz: "🗜️",
                txt: "📄", json: "📄", xml: "📄",
                mp3: "🎵", wav: "🎵", ogg: "🎵",
                mp4: "🎬", avi: "🎬", mkv: "🎬", mov: "🎬",
                default: "📁"
            };
            
            if (type === 2) { // 目录
                return "📂";
            }
            
            return icons[ext] || icons.default;
        }
        
        // 格式化文件大小
        function formatFileSize(bytes) {
            if (typeof bytes !== 'number' || bytes < 0) return '未知大小';
            if (bytes === 0) return '0 Bytes';
            
            const k = 1024;
            const sizes = ['Bytes', 'KB', 'MB', 'GB', 'TB'];
            const i = Math.floor(Math.log(bytes) / Math.log(k));
            
            return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
        }
        
        // 格式化日期
        function formatDate(timestamp) {
            if (!timestamp) return '未知日期';
            const date = new Date(timestamp * 1000); // 假设timestamp是秒级时间戳
            if (isNaN(date.getTime())) return '未知日期';
            return date.toLocaleDateString() + ' ' + date.toLocaleTimeString();
        }
        
        // 下载文件
        function downloadFile(url, filename) {
            if (!url) return;
            
            // 创建隐藏的下载链接并触发点击
            const a = document.createElement('a');
            a.href = encodeURIComponent(url);
            a.style.display = 'none';
            // 媒体文件使用新标签页打开
            const mediaExtensions = ['mp4', 'mp3', 'wav', 'ogg', 'avi', 'mkv', 'mov'];
            const ext = (filename || url).split('.').pop().toLowerCase();
            if (mediaExtensions.includes(ext)) {
                a.target = '_blank';
            } else {
                a.download = filename || url.split('/').pop();
            }
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
            
            // 可选: 记录下载事件
            console.log(`下载文件: ${url}`);
        }
        
        // 渲染文件列表
        function renderFileList(files) {
            const container = document.getElementById('fileListContainer');
            
            if (!files || files.length === 0) {
                container.innerHTML = '<div class="loading">没有找到文件</div>';
                return;
            }
            
            const list = document.createElement('ul');
            list.className = 'file-list';
            
            files.forEach(file => {
                const item = document.createElement('li');
                item.className = 'file-item';
                
                // 确保文件对象有必要的属性
                const name = file.name || file.filename || '未命名文件';
                const size = formatFileSize(file.size || file.fileSize);
                const date = formatDate(file.date || file.modified || file.createdAt);
                const url = file.url || file.downloadUrl || file.path;
                
                if (file.type == 1) {
                    item.innerHTML = `
                        <span class="file-icon">${getFileIcon(name, 1)}</span>
                        <span class="file-name" title="${name}">${name}</span>
                        <div class="file-info">
                            <span class="file-size">${size}</span>
                            <span class="file-date">${date}</span>
                        </div>
                        <button class="download-btn" title="下载" onclick=\"downloadFile('${url}', '${name.replace(/'/g, "\\'")}')\">
                            ↓
                        </button>
                    `;

                    // 点击文件名也可以下载
                    item.querySelector('.file-name').addEventListener('click', () => {
                        downloadFile(url, name);
                    });
                }
                else if (file.type == 2) { // 目录
                    item.className += ' directory';
                    item.innerHTML = `
                        <span class="file-icon">${getFileIcon(name, 2)}</span>
                        <span class="file-name" title="${name}">${name}</span>
                        <div class="file-info">
                            <span class="file-size">文件夹</span>
                            <span class="file-date">${date}</span>
                        </div>
                        <span class="download-btn"></span>
                    `;
                    // 点击文件名也可以下载
                    item.querySelector('.file-name').addEventListener('click', () => {
                        console.log('点击目录:', name);
                        apiUrl = apiUrl + name + '/';
                        fetchFileList();
                    });
                }
                
                list.appendChild(item);
            });
            
            container.innerHTML = '';
            container.appendChild(list);
            document.getElementById('dirName').innerText = basePath + apiUrl.substring(apiPrefix.length);
            document.getElementById('fileCount').innerText = files.length;

            if (apiUrl != apiPrefix) {
                document.getElementById('backBtn').style.display = 'block';
                document.getElementById('backBtn').addEventListener('click', () => {
                    if (processing) return; // 防止重复点击
                    processing = true;
                    // 去掉最后一个斜杠
                    if (apiUrl.length > apiPrefix.length) {
                        if (apiUrl.endsWith('/')) {
                            apiUrl = apiUrl.slice(0, apiUrl.length - 1);
                        }
                        apiUrl = apiUrl.substring(0, apiUrl.lastIndexOf('/')) + '/';
                    }

                    console.log('返回上级目录:', apiUrl);
                    if (apiUrl == apiPrefix) {
                        document.getElementById('backBtn').style.display = 'none';
                    }
                    fetchFileList();
                });
            } else {
                document.getElementById('backBtn').style.display = 'none';
            }
            processing = false; // 重置处理状态
        }
        
        // 初始化页面
        document.addEventListener('DOMContentLoaded', () => {
            // 首次加载文件列表
            fetchFileList();
            
            // 添加刷新按钮事件
            document.getElementById('refreshBtn').addEventListener('click', fetchFileList);
        });
    </script>
</body>
</html>
)";

const std::string_view upload_html_text = 
R"(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>支持断点续传的文件上传系统</title>
    <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/css/all.min.css">
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
        }
        
        body {
            background: linear-gradient(135deg, #6a11cb 0%, #2575fc 100%);
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
            padding: 20px;
        }
        
        .container {
            width: 100%;
            max-width: 900px;
            background-color: rgba(255, 255, 255, 0.95);
            border-radius: 20px;
            box-shadow: 0 15px 30px rgba(0, 0, 0, 0.2);
            overflow: hidden;
        }
        
        .header {
            background: linear-gradient(to right, #4a00e0, #8e2de2);
            color: white;
            padding: 25px 30px;
            text-align: center;
        }
        
        .header h1 {
            font-size: 28px;
            margin-bottom: 10px;
            display: flex;
            align-items: center;
            justify-content: center;
            gap: 10px;
        }
        
        .header p {
            opacity: 0.9;
            font-size: 16px;
        }
        
        .upload-container {
            padding: 30px;
        }
        
        .upload-area {
            border: 3px dashed #8e2de2;
            border-radius: 15px;
            padding: 40px 20px;
            text-align: center;
            cursor: pointer;
            transition: all 0.3s ease;
            background-color: #f8f9ff;
            margin-bottom: 25px;
        }
        
        .upload-area:hover, .upload-area.dragover {
            background-color: #eef2ff;
            border-color: #4a00e0;
        }
        
        .upload-icon {
            font-size: 60px;
            color: #8e2de2;
            margin-bottom: 15px;
        }
        
        .upload-area h3 {
            color: #333;
            margin-bottom: 10px;
            font-size: 22px;
        }
        
        .upload-area p {
            color: #666;
            margin-bottom: 20px;
            font-size: 16px;
        }
        
        .browse-btn {
            background: linear-gradient(to right, #4a00e0, #8e2de2);
            color: white;
            border: none;
            padding: 12px 30px;
            border-radius: 50px;
            font-size: 16px;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.3s ease;
            box-shadow: 0 4px 15px rgba(142, 45, 226, 0.3);
        }
        
        .browse-btn:hover {
            transform: translateY(-3px);
            box-shadow: 0 7px 20px rgba(142, 45, 226, 0.4);
        }
        
        .file-input {
            display: none;
        }
        
        .uploaded-files {
            margin-top: 30px;
        }
        
        .uploaded-files h3 {
            color: #333;
            margin-bottom: 15px;
            font-size: 20px;
            display: flex;
            align-items: center;
            gap: 10px;
        }
        
        .file-list {
            list-style: none;
        }
        
        .file-item {
            display: flex;
            align-items: center;
            padding: 15px;
            background-color: white;
            border-radius: 10px;
            margin-bottom: 15px;
            box-shadow: 0 3px 10px rgba(0, 0, 0, 0.08);
            transition: all 0.3s ease;
        }
        
        .file-item:hover {
            transform: translateY(-3px);
            box-shadow: 0 5px 15px rgba(0, 0, 0, 0.1);
        }
        
        .file-icon {
            font-size: 24px;
            margin-right: 15px;
            color: #8e2de2;
        }
        
        .file-info {
            flex: 1;
        }
        
        .file-name {
            font-weight: 600;
            color: #333;
            margin-bottom: 5px;
        }
        
        .file-size {
            color: #777;
            font-size: 14px;
        }
        
        .file-actions {
            display: flex;
            gap: 10px;
        }
        
        .file-action-btn {
            background: none;
            border: none;
            color: #777;
            cursor: pointer;
            font-size: 16px;
            transition: all 0.3s ease;
            padding: 5px 10px;
            border-radius: 5px;
        }
        
        .file-action-btn:hover {
            color: #8e2de2;
            background-color: #f0f0f0;
        }
        
        .progress-container {
            width: 100%;
            height: 6px;
            background-color: #e0e0e0;
            border-radius: 3px;
            margin-top: 10px;
            overflow: hidden;
        }
        
        .progress-bar {
            height: 100%;
            background: linear-gradient(to right, #4a00e0, #8e2de2);
            width: 0%;
            transition: width 0.4s ease;
        }
        
        .upload-btn {
            width: 100%;
            background: linear-gradient(to right, #4a00e0, #8e2de2);
            color: white;
            border: none;
            padding: 16px;
            border-radius: 10px;
            font-size: 18px;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.3s ease;
            margin-top: 20px;
            box-shadow: 0 4px 15px rgba(142, 45, 226, 0.3);
        }
        
        .upload-btn:hover {
            transform: translateY(-3px);
            box-shadow: 0 7px 20px rgba(142, 45, 226, 0.4);
        }
        
        .upload-btn:disabled {
            background: #cccccc;
            cursor: not-allowed;
            transform: none;
            box-shadow: none;
        }
        
        .preview-container {
            margin-top: 20px;
            display: none;
        }
        
        .preview-image {
            max-width: 100%;
            max-height: 200px;
            border-radius: 10px;
            box-shadow: 0 3px 10px rgba(0, 0, 0, 0.1);
        }
        
        .upload-status {
            margin-top: 10px;
            font-size: 14px;
            color: #666;
        }
        
        .chunk-info {
            display: flex;
            flex-wrap: wrap;
            gap: 5px;
            margin-top: 10px;
        }
        
        .chunk {
            width: 20px;
            height: 10px;
            background-color: #e0e0e0;
            border-radius: 2px;
            transition: background-color 0.3s ease;
        }
        
        .chunk.uploaded {
            background-color: #4a00e0;
        }
        
        .chunk.uploading {
            background-color: #8e2de2;
            animation: pulse 1.5s infinite;
        }
        
        @keyframes pulse {
            0% { opacity: 0.6; }
            50% { opacity: 1; }
            100% { opacity: 0.6; }
        }
        
        .resume-info {
            font-size: 12px;
            color: #4a00e0;
            margin-top: 5px;
        }
        
        .debug-info {
            margin-top: 15px;
            padding: 10px;
            background: #f8f9fa;
            border-radius: 5px;
            border-left: 4px solid #8e2de2;
            font-size: 12px;
            color: #666;
            display: none;
            max-height: 200px;
            overflow-y: auto;
            white-space: pre-wrap;
            font-family: monospace;
        }
        
        @media (max-width: 600px) {
            .container {
                border-radius: 15px;
            }
            
            .header {
                padding: 20px;
            }
            
            .header h1 {
                font-size: 24px;
            }
            
            .upload-container {
                padding: 20px;
            }
            
            .upload-area {
                padding: 30px 15px;
            }
            
            .file-actions {
                flex-direction: column;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1><i class="fas fa-cloud-upload-alt"></i> 支持断点续传的文件上传系统</h1>
            <p>大文件分块上传，支持暂停和恢复上传</p>
        </div>
        
        <div class="upload-container">
            <div class="upload-area" id="uploadArea">
                <i class="fas fa-cloud-upload-alt upload-icon"></i>
                <h3>拖放文件到此处</h3>
                <p>支持大文件上传，自动分块并支持断点续传</p>
                <button class="browse-btn">选择文件</button>
                <input type="file" class="file-input" id="fileInput">
            </div>
            
            <div class="preview-container" id="previewContainer">
                <h3><i class="fas fa-image"></i> 图片预览</h3>
                <img class="preview-image" id="previewImage" src="" alt="预览图片">
            </div>
            
            <div class="uploaded-files">
                <h3><i class="fas fa-file"></i> 已选择文件</h3>
                <ul class="file-list" id="fileList"></ul>
            </div>
            
            <button class="upload-btn" id="uploadBtn" disabled>开始上传</button>
            
            <div class="debug-info" id="debugInfo">
                <strong>HTTP请求信息：</strong><br>
                <span id="debugContent">等待上传...</span>
            </div>
        </div>
    </div>

    <script>
        document.addEventListener('DOMContentLoaded', function() {
            const uploadArea = document.getElementById('uploadArea');
            const fileInput = document.getElementById('fileInput');
            const fileList = document.getElementById('fileList');
            const uploadBtn = document.getElementById('uploadBtn');
            const previewContainer = document.getElementById('previewContainer');
            const previewImage = document.getElementById('previewImage');
            const browseBtn = uploadArea.querySelector('.browse-btn');
            const debugInfo = document.getElementById('debugInfo');
            const debugContent = document.getElementById('debugContent');
            
            // ==================== 配置区域 ====================
            // 请在这里填写您的实际上传URL
            const UPLOAD_URL = "/upload"; // 测试用的URL，请替换为您的实际上传URL
            // ==================== 配置结束 ====================
            
            // 上传状态管理
            const uploadState = {
                file: null,
                uploadId: null,
                chunkSize: 2 * 1024 * 1024, // 2MB 分块大小
                uploadedChunks: [],
                isUploading: false,
                isPaused: false,
                currentChunk: 0,
                totalChunks: 0,
                xhr: null,
            };
            
            // 点击上传区域触发文件选择
            browseBtn.addEventListener('click', function(e) {
                e.stopPropagation();
                fileInput.click();
            });
            
            // 文件选择处理
            fileInput.addEventListener('change', function() {
                if (this.files.length > 0) {
                    handleFile(this.files[0]);
                }
            });
            
            // 拖放事件处理
            uploadArea.addEventListener('dragover', function(e) {
                e.preventDefault();
                this.classList.add('dragover');
            });
            
            uploadArea.addEventListener('dragleave', function() {
                this.classList.remove('dragover');
            });
            
            uploadArea.addEventListener('drop', function(e) {
                e.preventDefault();
                this.classList.remove('dragover');
                if (e.dataTransfer.files.length > 0) {
                    handleFile(e.dataTransfer.files[0]);
                }
            });
            
            // 处理选择的文件
            async function handleFile(file) {
                uploadState.file = file;
                uploadState.uploadId = generateUploadId();
                uploadState.uploadedChunks = [];
                uploadState.currentChunk = 0;
                uploadState.totalChunks = Math.ceil(file.size / uploadState.chunkSize);
                uploadState.isUploading = false;
                uploadState.isPaused = false;
                
                renderFileItem();
                uploadBtn.disabled = false;
                uploadBtn.textContent = '开始上传';
                
                // 显示调试信息
                debugInfo.style.display = 'block';
                debugContent.textContent = `准备上传文件: ${file.name}\n文件大小: ${formatFileSize(file.size)}\n分块数: ${uploadState.totalChunks}`;
                
                // 如果有图片文件，显示预览
                if (file.type.startsWith('image/')) {
                    const reader = new FileReader();
                    reader.onload = function(e) {
                        previewImage.src = e.target.result;
                        previewContainer.style.display = 'block';
                    };
                    reader.readAsDataURL(file);
                } else {
                    previewContainer.style.display = 'none';
                }
                
                // 尝试恢复之前的进度
                const savedProgress = localStorage.getItem(uploadState.uploadId);
                if (savedProgress) {
                    const progress = JSON.parse(savedProgress);
                    uploadState.uploadedChunks = progress.uploadedChunks || [];
                    uploadState.currentChunk = progress.currentChunk || 0;
                    
                    // 更新UI显示恢复信息
                    const fileItem = document.querySelector('.file-item');
                    if (fileItem) {
                        const resumeInfo = document.createElement('div');
                        resumeInfo.className = 'resume-info';
                        resumeInfo.textContent = `检测到上传进度 ${Math.round((uploadState.uploadedChunks.length / uploadState.totalChunks) * 100)}%，可继续上传`;
                        fileItem.querySelector('.file-info').appendChild(resumeInfo);
                        
                        // 更新进度条
                        const progressBar = fileItem.querySelector('.progress-bar');
                        progressBar.style.width = `${(uploadState.uploadedChunks.length / uploadState.totalChunks) * 100}%`;
                        
                        // 更新分块显示
                        updateChunkDisplay(fileItem);
                    }
                }
            }
            
            // 生成唯一上传ID
            function generateUploadId() {
                return 'upload_' + Date.now() + '_' + Math.random().toString(36).substr(2, 9);
            }
            
            // 渲染文件项
            function renderFileItem() {
                fileList.innerHTML = '';
                
                const listItem = document.createElement('li');
                listItem.className = 'file-item';
                
                const fileSize = formatFileSize(uploadState.file.size);
                const fileExtension = getFileExtension(uploadState.file.name);
                
                listItem.innerHTML = `
                    <i class="fas ${getFileIcon(fileExtension)} file-icon"></i>
                    <div class="file-info">
                        <div class="file-name">${uploadState.file.name}</div>
                        <div class="file-size">${fileSize} · 分块数: ${uploadState.totalChunks}</div>
                        <div class="upload-status" id="uploadStatus">准备上传</div>
                        <div class="chunk-info" id="chunkInfo"></div>
                        <div class="progress-container">
                            <div class="progress-bar" id="progressBar"></div>
                        </div>
                    </div>
                    <div class="file-actions">
                        <button class="file-action-btn" id="pauseBtn" title="暂停上传">
                            <i class="fas fa-pause"></i>
                        </button>
                        <button class="file-action-btn" onclick="removeFile()\" title="移除文件">
                            <i class="fas fa-times"></i>
                        </button>
                    </div>
                `;
                
                fileList.appendChild(listItem);
                
                // 初始化分块显示
                initChunkDisplay(listItem);
                
                // 暂停按钮事件
                document.getElementById('pauseBtn').addEventListener('click', togglePauseUpload);
            }
            
            // 初始化分块显示
            function initChunkDisplay(fileItem) {
                const chunkInfo = fileItem.querySelector('#chunkInfo');
                chunkInfo.innerHTML = '';
                
                for (let i = 0; i < uploadState.totalChunks; i++) {
                    const chunk = document.createElement('div');
                    chunk.className = 'chunk';
                    chunkInfo.appendChild(chunk);
                }
                
                // 更新已上传的分块
                updateChunkDisplay(fileItem);
            }
            
            // 更新分块显示
            function updateChunkDisplay(fileItem) {
                const chunks = fileItem.querySelectorAll('.chunk');
                
                chunks.forEach((chunk, index) => {
                    chunk.classList.remove('uploaded', 'uploading');
                    
                    if (uploadState.uploadedChunks.includes(index)) {
                        chunk.classList.add('uploaded');
                    } else if (index === uploadState.currentChunk && uploadState.isUploading && !uploadState.isPaused) {
                        chunk.classList.add('uploading');
                    }
                });
            }
            
            // 获取文件扩展名
            function getFileExtension(filename) {
                return filename.slice((filename.lastIndexOf('.') - 1 >>> 0) + 2);
            }
            
            // 根据文件类型获取图标
            function getFileIcon(extension) {
                const iconMap = {
                    'pdf': 'fa-file-pdf',
                    'doc': 'fa-file-word',
                    'docx': 'fa-file-word',
                    'xls': 'fa-file-excel',
                    'xlsx': 'fa-file-excel',
                    'ppt': 'fa-file-powerpoint',
                    'pptx': 'fa-file-powerpoint',
                    'zip': 'fa-file-archive',
                    'rar': 'fa-file-archive',
                    'jpg': 'fa-file-image',
                    'jpeg': 'fa-file-image',
                    'png': 'fa-file-image',
                    'gif': 'fa-file-image',
                    'txt': 'fa-file-alt',
                    'mp4': 'fa-file-video',
                    'mp3': 'fa-file-audio'
                };
                
                return iconMap[extension.toLowerCase()] || 'fa-file';
            }
            
            // 格式化文件大小
            function formatFileSize(bytes) {
                if (bytes === 0) return '0 Bytes';
                
                const k = 1024;
                const sizes = ['Bytes', 'KB', 'MB', 'GB'];
                const i = Math.floor(Math.log(bytes) / Math.log(k));
                
                return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
            }
            
            // 移除文件
            window.removeFile = function() {
                if (uploadState.isUploading && !uploadState.isPaused) {
                    if (!confirm('文件正在上传，确定要移除吗？')) {
                        return;
                    }
                    
                    // 取消上传
                    if (uploadState.xhr) {
                        uploadState.xhr.abort();
                    }
                }
                
                // 清理本地存储
                if (uploadState.uploadId) {
                    localStorage.removeItem(uploadState.uploadId);
                }
                
                uploadState.file = null;
                uploadState.uploadId = null;
                uploadState.uploadedChunks = [];
                uploadState.isUploading = false;
                uploadState.isPaused = false;
                uploadState.xhr = null;
                
                fileList.innerHTML = '';
                previewContainer.style.display = 'none';
                uploadBtn.disabled = true;
                uploadBtn.textContent = '开始上传';
                debugInfo.style.display = 'none';
            };
            
            // 暂停/恢复上传
            function togglePauseUpload() {
                if (!uploadState.isUploading) return;
                
                uploadState.isPaused = !uploadState.isPaused;
                const pauseBtn = document.getElementById('pauseBtn');
                
                if (uploadState.isPaused) {
                    pauseBtn.innerHTML = '<i class="fas fa-play"></i>';
                    pauseBtn.title = '继续上传';
                    document.getElementById('uploadStatus').textContent = '已暂停';
                    
                    // 取消当前请求
                    if (uploadState.xhr) {
                        uploadState.xhr.abort();
                    }
                } else {
                    pauseBtn.innerHTML = '<i class="fas fa-pause"></i>';
                    pauseBtn.title = '暂停上传';
                    document.getElementById('uploadStatus').textContent = '上传中...';
                    // 继续上传
                    uploadChunks();
                }
                
                updateChunkDisplay(document.querySelector('.file-item'));
            }
            
            // 上传文件
            uploadBtn.addEventListener('click', function() {
                if (!uploadState.file) return;
                
                if (!uploadState.isUploading) {
                    // 开始上传
                    uploadState.isUploading = true;
                    uploadBtn.textContent = '上传中...';
                    uploadBtn.disabled = true;
                    
                    document.getElementById('uploadStatus').textContent = '上传中...';
                    uploadChunks();
                }
            });
            
            // 分块上传
            async function uploadChunks() {
                const fileItem = document.querySelector('.file-item');
                const progressBar = document.getElementById('progressBar');
                const uploadStatus = document.getElementById('uploadStatus');
                
                while (uploadState.currentChunk < uploadState.totalChunks && 
                       uploadState.isUploading && !uploadState.isPaused) {
                    
                    // 如果这个分块已经上传过，跳过
                    if (uploadState.uploadedChunks.includes(uploadState.currentChunk)) {
                        uploadState.currentChunk++;
                        continue;
                    }
                    
                    // 计算当前分块的起始和结束位置
                    const start = uploadState.currentChunk * uploadState.chunkSize;
                    const end = Math.min(start + uploadState.chunkSize, uploadState.file.size);
                    const chunk = uploadState.file.slice(start, end);
                    
                    // 更新UI
                    uploadStatus.textContent = `上传分块 ${uploadState.currentChunk + 1}/${uploadState.totalChunks}`;
                    updateChunkDisplay(fileItem);
                    
                    try {
                        // 执行实际上传
                        await uploadChunk(chunk, uploadState.currentChunk);
                        
                        // 上传成功
                        uploadState.uploadedChunks.push(uploadState.currentChunk);
                        
                        // 保存进度到本地存储
                        localStorage.setItem(uploadState.uploadId, JSON.stringify({
                            uploadedChunks: uploadState.uploadedChunks,
                            currentChunk: uploadState.currentChunk,
                            fileName: uploadState.file.name,
                            fileSize: uploadState.file.size,
                            totalChunks: uploadState.totalChunks
                        }));
                        
                        // 更新进度条
                        const progress = (uploadState.uploadedChunks.length / uploadState.totalChunks) * 100;
                        progressBar.style.width = `${progress}%`;
                        
                        uploadState.currentChunk++;
                    } catch (error) {
                        console.error('上传分块失败:', error);
                        uploadStatus.textContent = `分块 ${uploadState.currentChunk + 1} 上传失败，重试中...`;
                        // 失败时不增加currentChunk，下次会重试这个分块
                    }
                }
                
                // 检查是否全部上传完成
                if (uploadState.uploadedChunks.length === uploadState.totalChunks) {
                    uploadStatus.textContent = '上传完成！';
                    uploadBtn.textContent = '上传完成';
                    uploadBtn.style.background = 'linear-gradient(to right, #00b09b, #96c93d)';
                    
                    // 清理本地存储
                    localStorage.removeItem(uploadState.uploadId);
                    
                    // 更新分块显示
                    updateChunkDisplay(fileItem);
                    
                    // 3秒后重置按钮
                    setTimeout(() => {
                        uploadBtn.textContent = '开始上传';
                        uploadBtn.style.background = 'linear-gradient(to right, #4a00e0, #8e2de2)';
                        uploadBtn.disabled = false;
                        uploadState.isUploading = false;
                    }, 3000);
                } else if (uploadState.isPaused) {
                    uploadStatus.textContent = '已暂停';
                    uploadBtn.disabled = false;
                    uploadBtn.textContent = '继续上传';
                } else if (uploadState.currentChunk >= uploadState.totalChunks) {
                    // 所有分块已处理但可能还有失败的需要重试
                    uploadStatus.textContent = '上传完成，正在处理失败的分块...';
                    uploadState.currentChunk = 0;
                    // 重新开始处理未完成的分块
                    setTimeout(uploadChunks, 1000);
                }
            }
            
            // 实际上传分块
            function uploadChunk(chunk, chunkIndex) {
                return new Promise((resolve, reject) => {
                    const formData = new FormData();
                    formData.append('file', chunk);
                    formData.append('chunkindex', chunkIndex);
                    formData.append('totalchunks', uploadState.totalChunks);
                    formData.append('uploadid', uploadState.uploadId);
                    formData.append('filename', uploadState.file.name);
                    formData.append('filesize', uploadState.file.size);
                    
                    uploadState.xhr = new XMLHttpRequest();
                    
                    // 显示HTTP请求信息
                    const requestInfo = `=== HTTP请求信息 ===
请求方法: POST
请求URL: ${UPLOAD_URL}
Content-Type: multipart/form-data

=== 请求参数 ===
uploadId: ${uploadState.uploadId}
fileName: ${uploadState.file.name}
fileSize: ${uploadState.file.size}
chunkIndex: ${chunkIndex}
totalChunks: ${uploadState.totalChunks}
chunkSize: ${chunk.size}

=== 请求头 ===
${getRequestHeaders()}

开始上传分块 ${chunkIndex + 1}/${uploadState.totalChunks}...`;
                    
                    debugContent.textContent = requestInfo;
                    
                    uploadState.xhr.upload.addEventListener('progress', function(e) {
                        if (e.lengthComputable) {
                            const percentComplete = (e.loaded / e.total) * 100;
                            debugContent.textContent = requestInfo + `\n\n上传进度: ${percentComplete.toFixed(2)}%`;
                        }
                    });
                    
                    uploadState.xhr.addEventListener('load', function() {
                        if (uploadState.xhr.status >= 200 && uploadState.xhr.status < 300) {
                            const responseInfo = requestInfo + `\n\n=== 服务器响应 ===
状态码: ${uploadState.xhr.status}
响应头: ${getResponseHeaders(uploadState.xhr)}
响应体: ${uploadState.xhr.responseText.substring(0, 500)}...`;
                            
                            debugContent.textContent = responseInfo;
                            resolve();
                        } else {
                            const errorInfo = requestInfo + `\n\n=== 服务器响应 ===
状态码: ${uploadState.xhr.status}
响应头: ${getResponseHeaders(uploadState.xhr)}
错误信息: ${uploadState.xhr.statusText}`;
                            
                            debugContent.textContent = errorInfo;
                            reject(new Error(`上传失败: ${uploadState.xhr.status} ${uploadState.xhr.statusText}`));
                        }
                    });
                    
                    uploadState.xhr.addEventListener('error', function() {
                        const errorInfo = requestInfo + `\n\n=== 网络错误 ===
无法连接到服务器，请检查网络连接和URL配置`;
                        
                        debugContent.textContent = errorInfo;
                        reject(new Error('网络错误，上传失败'));
                    });
                    
                    uploadState.xhr.addEventListener('abort', function() {
                        const abortInfo = requestInfo + `\n\n=== 上传已取消 ===
用户取消了上传操作`;
                        
                        debugContent.textContent = abortInfo;
                        reject(new Error('上传已取消'));
                    });
                    
                    uploadState.xhr.open('POST', UPLOAD_URL);
                    uploadState.xhr.send(formData);
                });
            }
            
            // 获取请求头信息
            function getRequestHeaders() {
                const headers = [];
                headers.push('Content-Type: multipart/form-data');
                headers.push('X-Requested-With: XMLHttpRequest');
                headers.push('X-Upload-ID: ' + uploadState.uploadId);
                headers.push('X-File-Name: ' + encodeURIComponent(uploadState.file.name));
                headers.push('X-File-Size: ' + uploadState.file.size);
                headers.push('X-Chunk-Index: ' + uploadState.currentChunk);
                headers.push('X-Total-Chunks: ' + uploadState.totalChunks);
                headers.push('X-Chunk-Size: ' + uploadState.chunkSize);
                return headers.join('\n');
            }
            
            // 获取响应头信息
            function getResponseHeaders(xhr) {
                const headers = [];
                const headerString = xhr.getAllResponseHeaders();
                if (headerString) {
                    const lines = headerString.trim().split(/[\r\n]+/);
                    lines.forEach(line => {
                        const parts = line.split(': ');
                        const header = parts.shift();
                        const value = parts.join(': ');
                        headers.push(`${header}: ${value}`);
                    });
                }
                return headers.length > 0 ? headers.join('\n') : '无响应头信息';
            }
        });
    </script>
</body>
</html>
)";

}
