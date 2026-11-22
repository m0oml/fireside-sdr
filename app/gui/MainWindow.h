#pragma once
#include "fireside/sim_radio.h"
#include <QMainWindow>
#include <memory>
#include "fireside/sim_bus.h"
class QComboBox;
class QBoxLayout;
class QPlainTextEdit;
class QTabWidget;
class ReceiverPanel;
class RadioPanel;
class AntennaPanel;
class LogPanel;
class TxPanel; class TransmitCard;
class HardwareDayPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(bool enableAudio = true, bool threaded = true, bool simulate = false, QWidget* parent = nullptr);
    ~MainWindow() override;
    class PskPanel* pskPanel() const { return psk_panel_; }
    class CatPanel* catPanel() const { return cat_; }
    RadioPanel* radio() const { return radio_; }
    AntennaPanel* antennas() const { return antennas_; }
    fireside::sim::DiceBus* simBus() const { return sim_.get(); }          // non-null with --simulate
    ReceiverPanel* receiver() const { return receiver_; }
    LogPanel* logPanel() const { return log_panel_; }
    TxPanel* txPanel() const { return tx_panel_; }
    TransmitCard* transmitCard() const { return tx_card_; }
    class AudioPanel* audioPanel() const { return audio_panel_; }
    fireside::sim::RadioModel* simRadio() const { return sim_radio_.get(); }   // --simulate only          // the radio's transmit (ATU tune, power / SWR meters): through the session                    // the offline transmit lab: renders files only, not connected to the radio
    void showReceiver();
    void showRadio();
    void refreshDevices();
    QString deviceSummary() const;                 // used by --smoke
protected:
    void closeEvent(QCloseEvent*) override;
private slots:
    void identify();                               // read-only: never sends a write opcode (Flex is built with the default read-only Safety)
private:
    void showTrip(const QString& reason);                 // the red "TRANSMITTER STOPPED" box above the tabs, 5 s
    class QLabel* trip_ = nullptr; class QTimer* trip_timer_ = nullptr;
public:
    QLabel* tripBox() const { return trip_; }
private:
    void applyExternal(bool on, bool restoring = false);   // the decoder card's "External program" switch
    int ext_prev_decoder_ = 0; bool ext_prev_seq_ = true, ext_prev_upload_ = false, ext_prev_data_ = false;
    QComboBox* devices_; QPlainTextEdit* log_; QTabWidget* tabs_; ReceiverPanel* receiver_; RadioPanel* radio_ = nullptr; AntennaPanel* antennas_ = nullptr; LogPanel* log_panel_ = nullptr; class AudioPanel* audio_panel_ = nullptr; class PskPanel* psk_panel_ = nullptr; class CatPanel* cat_ = nullptr; QBoxLayout* device_col_ = nullptr; QWidget* device_tab_ = nullptr; TxPanel* tx_panel_ = nullptr; TransmitCard* tx_card_ = nullptr; std::unique_ptr<fireside::sim::DiceBus> sim_; std::unique_ptr<fireside::sim::RadioModel> sim_radio_;
};
