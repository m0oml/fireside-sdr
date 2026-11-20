#pragma once
#include <QWidget>
#include <QString>
#include <vector>
class QCheckBox;
class QComboBox;
class QLabel;

// The "Antennas" tab: which receive antenna to use on each band. When the dial moves into another band the panel asks for that band's antenna (antennaWanted); the RadioPanel
// switches it through the reviewed RxAntenna path. Table, default and the auto switch are remembered (QSettings "fireside/antennas"; not with FIRESIDE_NO_SETTINGS).
// Pure policy and display: this panel never talks to the radio itself.
class AntennaPanel : public QWidget {
    Q_OBJECT
public:
    explicit AntennaPanel(QWidget* parent = nullptr);
    QString antennaFor(double dialHz) const;            // "ANT1" "ANT2" "ANT3" "RX1IN": the band's choice, else the default
    bool autoSwitch() const;
    QString bandNameFor(double dialHz) const;           // "40 m", "Long / medium wave", or "other frequencies"
    void onDial(double dialHz);                         // the dial moved: update the display and, with auto switching on, ask for the antenna if it changed
    void setRadioRunning(bool on);                      // display only
    void setActiveAntenna(const QString& name);
    // The main screen's selector: this antenna for the band the dial is in (or the default outside the listed bands), switched now.
    void setAntennaForDial(double dialHz, const QString& antenna);
    QString activeAntenna() const { return active_; }
    // Transmit antenna per band: ANT1..ANT3 (1..3), "Default" = the TX default. Only ANT1 goes through the ATU.
    int txAntennaFor(double dialHz) const;
    void setTxAntennaForDial(double dialHz, int ant);
    QComboBox* txBox(int i) const { return tx_box_[size_t(i)]; }
    QComboBox* txDefaultBox() const { return tx_default_; }         // what the radio is really using (after a switch or at start-up); display only
    int bandCount() const { return int(band_box_.size()); }
    QComboBox* bandBox(int i) const { return band_box_[size_t(i)]; }       // row i: 0 = Default, 1..4 = ANT1 ANT2 ANT3 RX1 IN
    QComboBox* defaultBox() const { return default_; }
    QCheckBox* autoBox() const { return auto_; }
signals:
    void antennaWanted(const QString& antenna, const QString& why);       // why: "40 m"
    void choiceChanged();                                                 // the table / default / active antenna changed (the main selector redraws)
private:
    void changed();                                     // table, default or auto changed: save, re-evaluate
    void evaluate(bool force);
    void updateStatus();
    void load(); void save() const;
    QCheckBox* auto_; QComboBox* default_; QLabel* status_; std::vector<QComboBox*> band_box_; std::vector<QLabel*> band_label_;
    std::vector<QComboBox*> tx_box_; QComboBox* tx_default_ = nullptr;
    double dial_hz_ = 7.1e6; QString last_wanted_, active_; bool running_ = false;
};
