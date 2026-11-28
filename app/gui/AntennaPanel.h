#pragma once
#include <QWidget>
#include <QString>
#include <array>
#include <vector>
#include "fireside/txlines.h"
class QSpinBox;
class QCheckBox;
class QComboBox;
class QLabel;

// The "Antennas" tab: which receive and transmit antenna to use on each band: ANT1, ANT2, ANT3 (and RX1 IN for receive), or "Last" = the antenna last chosen
// on that band (with the main screen's selector). Moving the dial into another band always asks for that band's antenna (antennaWanted); the RadioPanel
// switches it through the reviewed RxAntenna path. Outside the listed bands the antenna stays as it is. Remembered in QSettings "fireside/antennas"
// (not with FIRESIDE_NO_SETTINGS). There is no default antenna and no automatic-switching tick box any more (user 2026-11-28).
// Pure policy and display: this panel never talks to the radio itself.
class AntennaPanel : public QWidget {
    Q_OBJECT
public:
    explicit AntennaPanel(QWidget* parent = nullptr);
    QString antennaFor(double dialHz) const;            // "ANT1" "ANT2" "ANT3" "RX1IN": the band's choice, else the default
    QString bandNameFor(double dialHz) const;           // "40 m", "Long / medium wave", or "other frequencies"
    void onDial(double dialHz);                         // the dial moved: update the display and, with auto switching on, ask for the antenna if it changed
    void setRadioRunning(bool on);                      // display only
    void setActiveAntenna(const QString& name);
    // The main screen's selector: this antenna for the band the dial is in (or the default outside the listed bands), switched now.
    void setAntennaForDial(double dialHz, const QString& antenna);
    QString activeAntenna() const { return active_; }
    // Transmit antenna per band: ANT1..ANT3 (1..3), or Last (the antenna last chosen on the band, ANT1 until one is). All three go through the ATU.
    int txAntennaFor(double dialHz) const;
    void setTxAntennaForDial(double dialHz, int ant);
    QComboBox* txBox(int i) const { return tx_box_[size_t(i)]; }
                                                        // setActiveAntenna: what the radio is really using (after a switch or at start-up); display only
    int bandCount() const { return int(band_box_.size()); }
    QComboBox* bandBox(int i) const { return band_box_[size_t(i)]; }       // row i: 0..3 = ANT1 ANT2 ANT3 RX1 IN, 4 = Last
    QString lastRxFor(double dialHz) const;             // the receive antenna last chosen on the dial's band ("" = none yet)
    // The rear TX outputs (TX1 red, TX2 white, TX3 yellow). Only "Band switch" is built: such a line is on while the dial is in a band ticked for it
    // (receive and transmit alike); Amplifier keying and Transverter are placeholders (shown, not selectable).
    fireside::LineConfig lineConfig() const;
    void setLineStates(int tx1, int tx2, int tx3);      // display: -1 not driven / unknown, 0 off, 1 on
    QComboBox* lineRoleBox(int line) const { return line_role_[size_t(line)]; }
    QCheckBox* lineBandBox(int band, int line) const { return line_band_[size_t(band)][size_t(line)]; }
    QSpinBox* settleBox() const { return settle_; }
    QString lineStatusText() const;
signals:
    void antennaWanted(const QString& antenna, const QString& why);       // why: "40 m"
    void choiceChanged();                                                 // the table / default / active antenna changed (the main selector redraws)
    void linesChanged();                                                  // a role, a band tick or the settle time changed (already saved)
private:
    void changed();                                     // table, default or auto changed: save, re-evaluate
    void evaluate(bool force);
    void updateStatus();
    void load(); void save() const;
    QLabel* status_; std::vector<QComboBox*> band_box_; std::vector<QLabel*> band_label_;
    std::vector<QComboBox*> tx_box_;
    std::vector<QString> last_rx_; std::vector<int> last_tx_; int last_tx_other_ = 1;   // per band: the antenna last chosen there (for "Last")
    std::array<QComboBox*, fireside::kTxLines> line_role_{}; std::array<QLabel*, fireside::kTxLines> line_hdr_{}, line_state_lbl_{};
    std::vector<std::array<QCheckBox*, fireside::kTxLines>> line_band_; QSpinBox* settle_ = nullptr; std::array<int, fireside::kTxLines> line_state_{{-1, -1, -1}};
    void linesEdited(); void updateLineColumns(); void updateLineStatus();
    double dial_hz_ = 7.1e6; QString last_wanted_, active_; bool running_ = false;
};
