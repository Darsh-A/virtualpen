#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "./ui_udevdialog.h"
#include <linux/uinput.h>
#include <unistd.h>
#include <QStyleFactory>
#include <QDialog>
#include <QMessageBox>
#include <QMetaObject>
#include <QtConcurrent/QtConcurrent>
#include <QDesktopServices>
#include <arpa/inet.h>
#include <libusb-1.0/libusb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "linux-adk.h"
#include "virtualstylus.h"
using namespace QtConcurrent;
using namespace std;
namespace fs = std::filesystem;
bool MainWindow::isDebugMode{ false };

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , selectedDeviceIdentifier("")
    , selectedDevice("")
    , usbDevices(new QMap<string, string>())
    , displayScreenTranslator(new DisplayScreenTranslator())
    , pressureTranslator(new PressureTranslator())
{
    settings = new QSettings(setting_org, setting_app);
    filePermissionValidator = new FilePermissionValidator();
    dialog = new QDialog();
    ui->setupUi(this);
    ui->deviceXSize->setValidator(new QIntValidator(1, max_device_size, this));
    ui->deviceYSize->setValidator(new QIntValidator(1, max_device_size, this));
    ui->wifiPortInput->setValidator(new QIntValidator(1, 65535, this));
    ui->wifiPortInput->setText(getSetting(wifi_port_setting_key, QVariant::fromValue(4545)).toString());
    initDisplayStyles();
    libUsbContext = libusb_init(NULL);
    updateUsbConnectButton();
    populateUsbDevicesList();
}

void MainWindow::captureStylusInput(){
    VirtualStylus* virtualStylus = new VirtualStylus(displayScreenTranslator, pressureTranslator);
    virtualStylus->initializeStylus();
    capture(selectedDevice, virtualStylus);
}

void MainWindow::captureWifiInput(int port){
    VirtualStylus* virtualStylus = new VirtualStylus(displayScreenTranslator, pressureTranslator);
    virtualStylus->initializeStylus();
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if(serverFd < 0){
        wifiRunning.store(false);
        QMetaObject::invokeMethod(ui->connectionStatusLabel, "setText", Qt::QueuedConnection,
                                  Q_ARG(QString, QString::fromUtf8("WiFi Error")));
        QMetaObject::invokeMethod(this, [this]() { updateUsbConnectButton(); }, Qt::QueuedConnection);
        return;
    }
    int reuse = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if(bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0){
        ::close(serverFd);
        wifiRunning.store(false);
        QMetaObject::invokeMethod(ui->connectionStatusLabel, "setText", Qt::QueuedConnection,
                                  Q_ARG(QString, QString::fromUtf8("WiFi Error")));
        QMetaObject::invokeMethod(this, [this]() { updateUsbConnectButton(); }, Qt::QueuedConnection);
        return;
    }
    if(listen(serverFd, 1) < 0){
        ::close(serverFd);
        wifiRunning.store(false);
        QMetaObject::invokeMethod(ui->connectionStatusLabel, "setText", Qt::QueuedConnection,
                                  Q_ARG(QString, QString::fromUtf8("WiFi Error")));
        QMetaObject::invokeMethod(this, [this]() { updateUsbConnectButton(); }, Qt::QueuedConnection);
        return;
    }

    while(wifiRunning.load()){
        int clientFd = accept(serverFd, nullptr, nullptr);
        if(clientFd < 0){
            continue;
        }
        QMetaObject::invokeMethod(ui->connectionStatusLabel, "setText", Qt::QueuedConnection,
                                  Q_ARG(QString, QString::fromUtf8("WiFi Connected!")));
        std::string pending;
        AccessoryEventData accessoryEventData{};
        char buffer[1024];
        while(wifiRunning.load()){
            ssize_t bytesRead = recv(clientFd, buffer, sizeof(buffer), 0);
            if(bytesRead <= 0){
                break;
            }
            pending.append(buffer, buffer + bytesRead);
            size_t newlinePos = 0;
            while((newlinePos = pending.find('\n')) != std::string::npos){
                std::string line = pending.substr(0, newlinePos);
                pending.erase(0, newlinePos + 1);
                if(parseAccessoryEventDataLine(line, &accessoryEventData)){
                    virtualStylus->handleAccessoryEventData(&accessoryEventData);
                }
            }
        }
        ::close(clientFd);
        QMetaObject::invokeMethod(ui->connectionStatusLabel, "setText", Qt::QueuedConnection,
                                  Q_ARG(QString, QString::fromUtf8("WiFi Listening...")));
    }
    ::close(serverFd);
    wifiRunning.store(false);
    QMetaObject::invokeMethod(this, [this]() { updateUsbConnectButton(); }, Qt::QueuedConnection);
    delete virtualStylus;
}

void MainWindow::populateUsbDevicesList(){
    ui->usbDevicesListWidget->clear();
    fetchUsbDevices();    
    foreach(const string key, usbDevices->keys()){
        ui->usbDevicesListWidget->addItem(QString::fromStdString(key));
    }
}


void MainWindow::on_usbDevicesListWidget_itemClicked(QListWidgetItem *item){
    selectedDeviceIdentifier = item->text().toStdString();
    selectedDevice = usbDevices->value(selectedDeviceIdentifier);
    loadDeviceConfig();
    updateUsbConnectButton();
}

bool MainWindow::canConnectUsb(){
    if(selectedDevice != "" &&
        ui->deviceXSize->hasAcceptableInput() &&
        ui->deviceYSize->hasAcceptableInput() &&
        displayScreenTranslator->size_x != -1 &&
        displayScreenTranslator->size_y != -1){
        return true;
    }
    else{
        return false;
    }
}

bool MainWindow::canStartWifi(){
    return ui->deviceXSize->hasAcceptableInput() &&
           ui->deviceYSize->hasAcceptableInput() &&
           ui->wifiPortInput->hasAcceptableInput();
}

void MainWindow::updateUsbConnectButton(){
    ui->connectUsbButton->setEnabled(canConnectUsb());
    ui->startWifiButton->setEnabled(canStartWifi() && !wifiRunning.load());
}


void MainWindow::on_connectUsbButton_clicked(){
    displayUDevPermissionFixIfNeeded();
    QFuture<void> ignored = QtConcurrent::run([this] { return captureStylusInput(); });
    ui->connectionStatusLabel->setText(QString::fromUtf8("Connected!"));
    ui->connectUsbButton->setEnabled(false);
    ui->refreshUsbDevices->setEnabled(false);
    ui->usbDevicesListWidget->setEnabled(false);
}

void MainWindow::on_startWifiButton_clicked(){
    displayUDevPermissionFixIfNeeded();
    int port = ui->wifiPortInput->text().toInt();
    wifiRunning.store(true);
    QFuture<void> ignored = QtConcurrent::run([this, port] { return captureWifiInput(port); });
    ui->connectionStatusLabel->setText(QString::fromUtf8("WiFi Listening..."));
    updateUsbConnectButton();
}

void MainWindow::fetchUsbDevices(){
    usbDevices->clear();
    libusb_device ** devicesList = NULL;
    ssize_t nbDevices = libusb_get_device_list(NULL, &devicesList);
    int err = 0;
    for(ssize_t i = 0; i < nbDevices; i++){
        libusb_device *dev = devicesList[i];
        struct libusb_device_descriptor desc;
        err = libusb_get_device_descriptor(dev, &desc);
        libusb_device_handle *handle = NULL;
        err = libusb_open(dev, &handle);
        if (err) {
            if(MainWindow::isDebugMode){
                printf("Unable to open device...\n");
            }
            continue;
        }
        unsigned char buf[100];
        int descLength = -1;
        descLength = libusb_get_string_descriptor_ascii(handle, desc.iManufacturer, buf, sizeof(buf));
        if(descLength < 0){
            continue;
        }
        string manufacturer = reinterpret_cast<char*>(buf);

        descLength = libusb_get_string_descriptor_ascii(handle, desc.iProduct, buf, sizeof(buf));
        if(descLength < 0){
            continue;
        }
        string product = reinterpret_cast<char*>(buf);

        descLength = libusb_get_string_descriptor_ascii(handle, desc.iSerialNumber, buf, sizeof(buf));
        if(descLength < 0){
            continue;
        }
        string serialNumber = reinterpret_cast<char*>(buf);

        std::ostringstream ss;
        ss<< std::hex << desc.idVendor << ":" << std::hex << desc.idProduct;
        string device = ss.str();

        usbDevices->insert(manufacturer + "-" + product + " (" + serialNumber + ")", device);
    }
}

void MainWindow::initDisplayStyles(){
    ui->displayStyleComboBox->addItem("  Stretched", static_cast<int>(DisplayStyle::stretched));
    ui->displayStyleComboBox->addItem("  Fixed", static_cast<int>(DisplayStyle::fixed));
}


void MainWindow::on_deviceXSize_editingFinished()
{
    displayScreenTranslator->size_x = ui->deviceXSize->text().toInt();
    setSetting(x_device_setting_key, displayScreenTranslator->size_x);
    updateUsbConnectButton();
}


void MainWindow::on_deviceYSize_editingFinished()
{
    displayScreenTranslator->size_y = ui->deviceYSize->text().toInt();
    setSetting(y_device_setting_key, displayScreenTranslator->size_y);
    updateUsbConnectButton();
}

void MainWindow::on_wifiPortInput_editingFinished()
{
    setSetting(wifi_port_setting_key, ui->wifiPortInput->text().toInt());
    updateUsbConnectButton();
}

void MainWindow::manageInputBoxStyle(QLineEdit * inputBox){
    if(inputBox->hasAcceptableInput()){
        inputBox->setStyleSheet("QLineEdit{border: 1px solid white}");
    }
    else{
        inputBox->setStyleSheet("QLineEdit{border: 1px solid red}");
    }
}


void MainWindow::on_displayStyleComboBox_currentIndexChanged(int index)
{
    int displayStyleInt = ui->displayStyleComboBox->currentData().toInt();
    displayScreenTranslator->displayStyle = static_cast<DisplayStyle>(displayStyleInt);
    setSetting(display_style_setting_key, displayStyleInt);
}

void MainWindow::on_pressureSensitivitySlider_valueChanged(int value)
{
    pressureTranslator->sensitivity = value;
    setSetting(pressure_sensitivity_setting_key, value);
}


void MainWindow::on_minimumPressureSlider_valueChanged(int value)
{
    pressureTranslator->minPressure = value;
    setSetting(min_pressure_setting_key, value);
}

void MainWindow::loadDeviceConfig(){
    displayScreenTranslator->size_x = getSetting(x_device_setting_key).toInt();
    displayScreenTranslator->size_y = getSetting(y_device_setting_key).toInt();
    int displayStyleInt = getSetting(display_style_setting_key).toInt();
    int index = ui->displayStyleComboBox->findData(displayStyleInt);
    if ( index != -1 ) {
        displayScreenTranslator->displayStyle = static_cast<DisplayStyle>(displayStyleInt);
        ui->displayStyleComboBox->setCurrentIndex(index);
    }
    else{
        displayScreenTranslator->displayStyle = DisplayStyle::stretched;
        ui->displayStyleComboBox->setCurrentIndex(ui->displayStyleComboBox->findData(static_cast<int>(DisplayStyle::stretched)));
    }

    ui->displayStyleComboBox->activated(0);
    pressureTranslator->minPressure = getSetting(min_pressure_setting_key, QVariant::fromValue(10)).toInt();
    ui->minimumPressureSlider->setValue(pressureTranslator->minPressure);
    pressureTranslator->sensitivity = getSetting(pressure_sensitivity_setting_key, QVariant::fromValue(50)).toInt();
    ui->pressureSensitivitySlider->setValue(pressureTranslator->sensitivity);
    ui->deviceXSize->setText(QString::number(displayScreenTranslator->size_x));
    ui->deviceYSize->setText(QString::number(displayScreenTranslator->size_y));
    ui->wifiPortInput->setText(getSetting(wifi_port_setting_key, QVariant::fromValue(4545)).toString());
    on_deviceXSize_selectionChanged();
    on_deviceYSize_selectionChanged();
}

QVariant MainWindow::getSetting(string settingKey){
    return settings->value(QString::fromStdString(selectedDeviceIdentifier + settingKey));
}


QVariant MainWindow::getSetting(string settingKey, QVariant defaultValue){
    QVariant value = getSetting(settingKey);
    return value.isNull() ? defaultValue : value;
}

void MainWindow::setSetting(string settingKey, QVariant value){
    return settings->setValue(QString::fromStdString(selectedDeviceIdentifier + settingKey), value);
}

void MainWindow::on_refreshUsbDevices_clicked()
{
    populateUsbDevicesList();    
}

void MainWindow::displayUDevPermissionFixIfNeeded(){
    bool canWriteToUInput = filePermissionValidator->canWriteToFile("/dev/uinput");
    bool canWriteToUsbDevice = canWriteToAnyUsbDevice();
    if(!canWriteToUInput || !canWriteToUsbDevice){
        displayFixForUDevPermissions();
    }
}

bool MainWindow::canWriteToAnyUsbDevice(){
    if(!usbDevices->empty()){
        return filePermissionValidator->anyFileWriteableRecursive("/dev/bus/usb/");
    }
    else{
        return true;
    }

}


void MainWindow::displayFixForUDevPermissions(){

    QWidget widget;
    Ui::udevdialog udevDialog;
    udevDialog.setupUi(dialog);
    dialog->exec();
}


void MainWindow::on_deviceXSize_selectionChanged()
{
    manageInputBoxStyle(ui->deviceXSize);
    updateUsbConnectButton();
}


void MainWindow::on_deviceYSize_selectionChanged()
{
        manageInputBoxStyle(ui->deviceYSize);
        updateUsbConnectButton();
}

void MainWindow::on_connectUsbButton_2_clicked()
{
    QString link = "https://github.com/androidvirtualpen/virtualpen/releases/download/0.1/virtual-pen.apk";
    QDesktopServices::openUrl(QUrl(link));
}

MainWindow::~MainWindow()
{
    delete ui;
    delete usbDevices;
    delete displayScreenTranslator;
    delete pressureTranslator;
    delete settings;
    delete filePermissionValidator;
}
