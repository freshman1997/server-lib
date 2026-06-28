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
            display: inline-flex;
            align-items: center;
            gap: 8px;
            padding: 6px 10px;
            border-radius: 999px;
            background: #f2f4ff;
        }

        .upload-status::before {
            content: '';
            width: 8px;
            height: 8px;
            border-radius: 50%;
            background: #8e2de2;
        }

        .upload-status.paused::before { background: #f59e0b; }
        .upload-status.error::before { background: #ef4444; }
        .upload-status.processing::before { background: #2563eb; animation: pulse 1.5s infinite; }
        .upload-status.done::before { background: #10b981; }

        .progress-meta {
            display: flex;
            justify-content: space-between;
            gap: 12px;
            color: #777;
            font-size: 12px;
            margin-top: 8px;
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
            padding: 8px 10px;
            border-radius: 8px;
            background: #f3e8ff;
        }

        .hint-info {
            margin-top: 12px;
            padding: 10px 12px;
            background: #eef2ff;
            color: #3730a3;
            border-radius: 10px;
            font-size: 13px;
            line-height: 1.5;
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
)"
R"(
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
                storageKey: null,
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
                uploadState.uploadId = await generateUploadId(file);
                uploadState.uploadedChunks = [];
                uploadState.currentChunk = 0;
                uploadState.totalChunks = Math.ceil(file.size / uploadState.chunkSize);
                uploadState.isUploading = false;
                uploadState.isPaused = false;
                uploadState.storageKey = getUploadStorageKey(uploadState.uploadId);
                
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
                const savedProgress = localStorage.getItem(uploadState.storageKey);
                if (savedProgress) {
                    let canResume = false;
                    try {
                        const progress = JSON.parse(savedProgress);
                        canResume = progress.fileName === file.name &&
                            progress.fileSize === file.size &&
                            progress.lastModified === file.lastModified &&
                            progress.chunkSize === uploadState.chunkSize &&
                            progress.totalChunks === uploadState.totalChunks;
                        if (canResume) {
                            uploadState.uploadedChunks = Array.isArray(progress.uploadedChunks) ? progress.uploadedChunks : [];
                            uploadState.currentChunk = progress.currentChunk || 0;
                        }
                    } catch (error) {
                        canResume = false;
                    }
                    if (!canResume) {
                        localStorage.removeItem(uploadState.storageKey);
                        return;
                    }
                    
                    // 更新UI显示恢复信息
                    const fileItem = document.querySelector('.file-item');
                    if (fileItem) {
                        const resumeInfo = document.createElement('div');
                        resumeInfo.className = 'resume-info';
                        resumeInfo.textContent = `检测到上传进度 ${Math.round((uploadState.uploadedChunks.length / uploadState.totalChunks) * 100)}%，可继续上传`;
                        fileItem.querySelector('.file-info').appendChild(resumeInfo);
                        
                        setProgress((uploadState.uploadedChunks.length / uploadState.totalChunks) * 100);
                        setHint('已恢复本地进度。服务器会校验文件名、大小和分片数，避免串文件。');
                        
                        // 更新分块显示
                        updateChunkDisplay(fileItem);
                    }
                }
            }
            
            function getUploadStorageKey(uploadId) {
                return 'upload-progress:' + uploadId;
            }

            function hashString(input) {
                let hash = 2166136261;
                for (let i = 0; i < input.length; i++) {
                    hash ^= input.charCodeAt(i);
                    hash = Math.imul(hash, 16777619);
                }
                return (hash >>> 0).toString(36);
            }

            async function sampleFileFingerprint(file) {
                if (!window.crypto || !crypto.subtle || !file.arrayBuffer) {
                    return 'meta_' + hashString([file.name, file.size, file.lastModified, uploadState.chunkSize].join('|'));
                }

                const sampleSize = Math.min(64 * 1024, file.size);
                const head = await file.slice(0, sampleSize).arrayBuffer();
                const tail = file.size > sampleSize
                    ? await file.slice(Math.max(0, file.size - sampleSize)).arrayBuffer()
                    : new ArrayBuffer(0);
                const meta = new TextEncoder().encode([file.name, file.size, file.lastModified, uploadState.chunkSize].join('|'));
                const bytes = new Uint8Array(meta.byteLength + head.byteLength + tail.byteLength);
                bytes.set(meta, 0);
                bytes.set(new Uint8Array(head), meta.byteLength);
                bytes.set(new Uint8Array(tail), meta.byteLength + head.byteLength);
                const digest = await crypto.subtle.digest('SHA-256', bytes);
                return Array.from(new Uint8Array(digest)).map(b => b.toString(16).padStart(2, '0')).join('').slice(0, 32);
            }

            // 生成稳定上传ID。同一文件重选/刷新后保持一致，才能真正断点续传。
            async function generateUploadId(file) {
                return 'file_' + await sampleFileFingerprint(file);
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
                        <div class="progress-meta">
                            <span id="progressText">0%</span>
                            <span id="speedText">等待开始</span>
                        </div>
                        <div class="hint-info" id="hintInfo">刷新或重新选择同一个文件后，可自动恢复已上传分片。</div>
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
                setStatus('ready', '准备上传');
                setProgress(0);
                
                // 暂停按钮事件
                document.getElementById('pauseBtn').addEventListener('click', togglePauseUpload);
            }

            function setStatus(kind, text) {
                const status = document.getElementById('uploadStatus');
                if (!status) return;
                status.className = 'upload-status';
                if (kind) status.classList.add(kind);
                status.textContent = text;
            }

            function setHint(text) {
                const hint = document.getElementById('hintInfo');
                if (hint) hint.textContent = text;
            }

            function setProgress(percent) {
                const value = Math.max(0, Math.min(100, percent));
                const progressBar = document.getElementById('progressBar');
                const progressText = document.getElementById('progressText');
                if (progressBar) progressBar.style.width = `${value}%`;
                if (progressText) progressText.textContent = `${value.toFixed(value === 100 ? 0 : 1)}%`;
            }

            function setSpeedText(text) {
                const speed = document.getElementById('speedText');
                if (speed) speed.textContent = text;
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
                if (uploadState.storageKey) {
                    localStorage.removeItem(uploadState.storageKey);
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
)"
R"(
            };
            
            // 暂停/恢复上传
            function togglePauseUpload() {
                if (!uploadState.isUploading) return;
                
                uploadState.isPaused = !uploadState.isPaused;
                const pauseBtn = document.getElementById('pauseBtn');
                
                if (uploadState.isPaused) {
                    pauseBtn.innerHTML = '<i class="fas fa-play"></i>';
                    pauseBtn.title = '继续上传';
                    setStatus('paused', '已暂停');
                    setHint('上传已暂停，进度已保存在浏览器本地。');
                    
                    // 取消当前请求
                    if (uploadState.xhr) {
                        uploadState.xhr.abort();
                    }
                } else {
                    pauseBtn.innerHTML = '<i class="fas fa-pause"></i>';
                    pauseBtn.title = '暂停上传';
                    setStatus('', '上传中...');
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
                    
                    setStatus('', '上传中...');
                    setHint('正在上传分片。网络中断或暂停后，可以继续上传未完成的分片。');
                    uploadChunks();
                }
            });
            
            // 分块上传
            async function uploadChunks() {
                const fileItem = document.querySelector('.file-item');
                let lastResponse = null;
                
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
                    setStatus('', `上传分块 ${uploadState.currentChunk + 1}/${uploadState.totalChunks}`);
                    updateChunkDisplay(fileItem);
                    
                    try {
                        // 执行实际上传
                        lastResponse = await uploadChunk(chunk, uploadState.currentChunk);
                        
                        // 上传成功
                        if (!uploadState.uploadedChunks.includes(uploadState.currentChunk)) {
                            uploadState.uploadedChunks.push(uploadState.currentChunk);
                        }
                        
                        // 保存进度到本地存储
                        localStorage.setItem(uploadState.storageKey, JSON.stringify({
                            uploadedChunks: uploadState.uploadedChunks,
                            currentChunk: uploadState.currentChunk,
                            fileName: uploadState.file.name,
                            fileSize: uploadState.file.size,
                            lastModified: uploadState.file.lastModified,
                            chunkSize: uploadState.chunkSize,
                            totalChunks: uploadState.totalChunks
                        }));
                        
                        // 更新进度条
                        setProgress((uploadState.uploadedChunks.length / uploadState.totalChunks) * 100);
                        setSpeedText(`${uploadState.uploadedChunks.length}/${uploadState.totalChunks} 分片完成`);
                        
                        uploadState.currentChunk++;
                    } catch (error) {
                        console.error('上传分块失败:', error);
                        setStatus('error', `分块 ${uploadState.currentChunk + 1} 上传失败，重试中...`);
                        setHint(error.message || '上传失败，稍后自动重试当前分片。');
                        // 失败时不增加currentChunk，下次会重试这个分块
                    }
                }
                
                // 检查是否全部上传完成
                if (uploadState.uploadedChunks.length === uploadState.totalChunks) {
                    const finalStatus = lastResponse && lastResponse.status ? lastResponse.status : 'processing';
                    if (finalStatus === 'complete') {
                        setStatus('done', '上传完成');
                        setHint('文件已合并完成，可以继续选择其他文件。');
                        uploadBtn.textContent = '上传完成';
                    } else {
                        setStatus('processing', '分片已上传，服务器正在合并...');
                        setHint('所有分片已到达服务器，后台正在合并文件。大文件可能需要一点时间。');
                        uploadBtn.textContent = '合并处理中';
                    }
                    uploadBtn.style.background = 'linear-gradient(to right, #00b09b, #96c93d)';
                    
                    // 清理本地存储
                    localStorage.removeItem(uploadState.storageKey);
                    
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
                    setStatus('paused', '已暂停');
                    uploadBtn.disabled = false;
                    uploadBtn.textContent = '继续上传';
                } else if (uploadState.currentChunk >= uploadState.totalChunks) {
                    // 所有分块已处理但可能还有失败的需要重试
                    setStatus('error', '正在重试未完成分片...');
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
                            let payload = {};
                            try {
                                payload = uploadState.xhr.responseText ? JSON.parse(uploadState.xhr.responseText) : {};
                            } catch (error) {
                                payload = {};
                            }
                            resolve(payload);
                        } else {
                            let message = uploadState.xhr.statusText || '上传失败';
                            try {
                                const payload = JSON.parse(uploadState.xhr.responseText || '{}');
                                if (payload.error) message = payload.error;
                            } catch (error) {
                            }
                            const errorInfo = requestInfo + `\n\n=== 服务器响应 ===
状态码: ${uploadState.xhr.status}
响应头: ${getResponseHeaders(uploadState.xhr)}
错误信息: ${message}`;
                             
                            debugContent.textContent = errorInfo;
                            reject(new Error(`上传失败: ${uploadState.xhr.status} ${message}`));
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
