#include <limitnvpstate.h>
#include <QSystemTrayIcon>
#include <nvidia.h>
#include <QMessageBox>
#include <config.h>
#include <utils.h>
#include <QDesktopServices>
#include <QFileDialog>

std::unordered_set<std::string> cachedProcessExceptions;
NvPhysicalGpuHandle hPhysicalGpus[NVAPI_MAX_PHYSICAL_GPUS];
HWINEVENTHOOK eventHook;

void WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    // this corrects the hwnd after a EVENT_SYSTEM_FOREGROUND event
    hwnd = GetForegroundWindow();

    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);

    std::string processName = toLower(getProcessNameByPID(pid));
    bool isProcessExcepted = cachedProcessExceptions.count(processName);

    std::cout << "info: " << processName << " is fg window (excepted: " << std::boolalpha << isProcessExcepted << ")\n";

    if (SetPState(hPhysicalGpus[config["gpu_index"]], isProcessExcepted, config["pstate_limit"]) != 0) {
        QMessageBox::critical(nullptr, "limit-nvpstate", "Error: Failed to set P-State");
        exit(1);
    }
}

limitnvpstate::limitnvpstate(QWidget* parent) : QMainWindow(parent) {
    ui.setupUi(this);
    createTrayIcon();

    if (initNvAPI() != 0) {
        QMessageBox::critical(nullptr, "limit-nvpstate", "Error: Failed to initialize NVAPI");
        exit(1);
    }

    unsigned long gpuCount = 0;
    if (NvAPI_EnumPhysicalGPUs(hPhysicalGpus, &gpuCount) != 0) {
        QMessageBox::critical(nullptr, "limit-nvpstate", "Error: Failed to enumerate physical GPUs");
        exit(1);
    }

    if (gpuCount == 0) {
        QMessageBox::critical(nullptr, "limit-nvpstate", "Error: No GPUs found");
        exit(1);
    }

    // file -> start minimized
    ui.actionStartMinimized->setChecked(config["start_minimized"]);

    connect(ui.actionStartMinimized, &QAction::triggered, [this]() {
        config["start_minimized"] = ui.actionStartMinimized->isChecked();
        saveConfig();
        });

    // file -> add to startup
    ui.actionAddToStartup->setChecked(isAddedToStartup());

    connect(ui.actionAddToStartup, &QAction::triggered, [this]() {
        addToStartup(ui.actionAddToStartup->isChecked());
        });

    // file -> exit
    connect(ui.actionExit, &QAction::triggered, [this]() {
        exit(1);
        });

    // help -> about
    connect(ui.actionAbout, &QAction::triggered, [this]() {
        QUrl url("https://github.com/valleyofdoom");
        QDesktopServices::openUrl(url);
        });

    // add GPUs to combobox

    for (unsigned int i = 0; i < gpuCount; ++i) {
        char gpuFullName[NVAPI_SHORT_STRING_MAX];

        if (NvAPI_GPU_GetFullName(hPhysicalGpus[i], gpuFullName) != 0) {
            QMessageBox::critical(nullptr, "limit-nvpstate", "Error: Failed to obtain GPU name");
            exit(1);
        }

        ui.selectedGPU->addItem(gpuFullName);
    }

    ui.selectedGPU->setCurrentIndex(config["gpu_index"]);
    connect(ui.selectedGPU, SIGNAL(currentIndexChanged(int)), this, SLOT(selectedGPUChanged(int)));

    NV_GPU_PERF_PSTATES20_INFO pStatesInfo;
    pStatesInfo.version = NV_GPU_PERF_PSTATES20_INFO_VER;

    if (NvAPI_GPU_GetPstates20(hPhysicalGpus[0], &pStatesInfo) != 0) {
        QMessageBox::critical(nullptr, "limit-nvpstate", "Error: Failed to obtain available P-States");
        exit(1);
    }

    for (int i = 1; i < pStatesInfo.numPstates; i++) {
        int pState = pStatesInfo.pstates[i].pstateId;
        ui.selectedPState->addItem(QString::fromStdString("P" + std::to_string(pState)));
    }

    std::string selectedPstate = "P" + std::to_string((int)config["pstate_limit"]);

    ui.selectedPState->setCurrentText(QString::fromStdString(selectedPstate));
    connect(ui.selectedPState, SIGNAL(currentIndexChanged(int)), this, SLOT(selectedPStateChanged(int)));

    // add process button
    for (std::string processException : config["process_exceptions"]) {
        ui.processExceptionsList->addItem(QString::fromStdString(processException));

        // when the foreground window is changed, the lowercase name of it is compared
        cachedProcessExceptions.insert(toLower(processException));
    }

    connect(ui.addProcess, &QPushButton::clicked, [this]() {
        addProcess();
        });

    // remove process button
    connect(ui.removeProcess, &QPushButton::clicked, [this]() {
        // Iterate through the selected items and remove them
        for (QListWidgetItem* processException : ui.processExceptionsList->selectedItems()) {
            delete ui.processExceptionsList->takeItem(ui.processExceptionsList->row(processException));
        }

        saveProcessExceptions();
        });

    // limit p-state initially
    if (SetPState(hPhysicalGpus[ui.selectedGPU->currentIndex()], false, config["pstate_limit"]) != 0) {
        QMessageBox::critical(nullptr, "limit-nvpstate", "Error: Failed to set P-State");
        exit(1);
    }

    eventHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);

    if (!eventHook) {
        QMessageBox::critical(nullptr, "limit-nvpstate", "Error: Failed to configure global hook");
        exit(1);
    }
}

void limitnvpstate::createTrayIcon() {
    QMenu* trayMenu = new QMenu(this);

    QAction* trayActionExit = new QAction("Exit", this);
    connect(trayActionExit, &QAction::triggered, [this]() {
        closeEvent(nullptr);
        exit(0);
        });
    trayMenu->addAction(trayActionExit);

    // create icon
    auto trayIcon = new QSystemTrayIcon(this);
    trayIcon->setContextMenu(trayMenu);
    trayIcon->setIcon(QIcon(":/limitnvpstate/icon.ico"));
    trayIcon->show();

    connect(trayIcon, &QSystemTrayIcon::activated, [this](QSystemTrayIcon::ActivationReason  reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            if (isVisible()) {
                hide();
            } else {
                showNormal();
                activateWindow();
            }
        }
        });
}

void limitnvpstate::selectedGPUChanged(int index) {
    config["gpu_index"] = index;
    saveConfig();
}

void limitnvpstate::selectedPStateChanged(int index) {
    std::string selected_pstate = ui.selectedPState->currentText().toStdString().substr(1);
    config["pstate_limit"] = std::stoi(selected_pstate);
    saveConfig();
}

void limitnvpstate::saveProcessExceptions() {
    // clear items in config
    config["process_exceptions"].clear();
    // clear internal list
    cachedProcessExceptions.clear();

    for (int i = 0; i < ui.processExceptionsList->count(); i++) {
        std::string processException = ui.processExceptionsList->item(i)->text().toStdString();

        // update config
        config["process_exceptions"].push_back(processException);

        // update internal list
        // when the foreground window is changed, the lowercase name of it is compared
        cachedProcessExceptions.insert(toLower(processException));
    }

    saveConfig();
}

void limitnvpstate::closeEvent(QCloseEvent* event) {
    if (SetPState(hPhysicalGpus[ui.selectedGPU->currentIndex()], true) != 0) {
        QMessageBox::critical(nullptr, "limit-nvpstate", "Error: Failed to set P-State");
        exit(1);
    }

    // cleanup
    UnhookWinEvent(eventHook);
}

void limitnvpstate::changeEvent(QEvent* e) {
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::WindowStateChange) {
        if (isMinimized()) {
            hide();
        }
    }
}

void limitnvpstate::addProcess() {
    QStringList selectedFilePaths = QFileDialog::getOpenFileNames(
        this,
        "Open File",
        "",
        tr("Executable Files (*.exe)")
    );

    if (selectedFilePaths.isEmpty()) {
        return;
    }

    for (QString selectedFilePath : selectedFilePaths) {
        std::string filePathStr = selectedFilePath.toStdString();

        // replace "/" with "\"
        std::replace(filePathStr.begin(), filePathStr.end(), '/', '\\');

        QString executableName = QString::fromStdString(getBaseName(filePathStr));

        // don't add if already in list to prevent duplicates
        if (ui.processExceptionsList->findItems(executableName, Qt::MatchExactly).isEmpty()) {
            ui.processExceptionsList->addItem(executableName);
        }
    }

    saveProcessExceptions();
}