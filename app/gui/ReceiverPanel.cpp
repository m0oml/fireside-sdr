#include "ReceiverPanel.h"
#include "fireside/award.h"
#include "fireside/tx_meter.h"
#include "fireside/bandplan.h"
#include "fireside/wspr.h"
#include "fireside/wspr_decode.h"
#include "AppLog.h"
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QScrollArea>
#include <QPointer>
#include <QApplication>
#include <QDateTime>
#include "fireside/qrz.h"
#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QTextCharFormat>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QTextBlock>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardItemModel>
#include <QSettings>
#include <QTextCursor>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QStyle>
#include <QRegularExpression>
#include <QFile>
#include <QVBoxLayout>
#include <QFontDatabase>
#include <algorithm>
#include <cmath>
#include <functional>
#include "AudioOut.h"
#include "FrequencyDisplay.h"
#include "SMeter.h"
#include "fireside/logbook.h"
#include "SpectrumWidget.h"
#include "WaterfallWidget.h"
#include "Theme.h"
#include "RecordController.h"
#include "fireside/capture.h"
#include "fireside/fw_iso.h"
#include "fireside/nr2.h"
#include "fireside/decode.h"
using namespace fireside;

namespace {
struct Band { const char* name; double start_hz, lo, hi; };
const Band kBands[] = {{"160", 1.9e6, 1.8e6, 2.0e6}, {"80", 3.7e6, 3.5e6, 4.0e6}, {"60", 5.357e6, 5.25e6, 5.45e6}, {"40", 7.1e6, 7.0e6, 7.3e6}, {"30", 10.12e6, 10.1e6, 10.15e6},
                       {"20", 14.2e6, 14.0e6, 14.35e6}, {"17", 18.1e6, 18.068e6, 18.168e6}, {"15", 21.2e6, 21.0e6, 21.45e6}, {"12", 24.9e6, 24.89e6, 24.99e6},
                       {"10", 28.4e6, 28.0e6, 29.7e6}, {"6", 50.15e6, 50.0e6, 54.0e6}};
const char* kModeNames[] = {"USB", "LSB", "AM", "CW", "FM"};
const char* kAgcNames[] = {"Off", "Fast", "Med", "Slow"};
QString width_text(double w) { return w < 1000 ? QString("%1 Hz").arg(w, 0, 'f', 0) : QString("%1k").arg(w / 1000.0, 0, 'g', 3); }
QPushButton* pill(const QString& text, bool checkable = true) { auto* b = new QPushButton(text); b->setObjectName("pill"); b->setCheckable(checkable); b->setCursor(Qt::PointingHandCursor); return b; }
QButtonGroup* group(QObject* parent, bool exclusive) { auto* g = new QButtonGroup(parent); g->setExclusive(exclusive); return g; }
}  // namespace

QFrame* ReceiverPanel::card(const QString& title, QLayout* body) { return theme::makeCard(title, body); }

ReceiverPanel::ReceiverPanel(bool enableAudio, bool threaded, QWidget* parent) : QWidget(parent), threaded_(threaded), audioOut_(new AudioOut(enableAudio, this)) {
    setObjectName("root"); band_mem_.resize(std::size(kBands)); fireside::dsp::enable_spectral_nr();
    auto* root = new QVBoxLayout(this); root->setContentsMargins(12, 12, 12, 8); root->setSpacing(12);

    // ---- header: frequency | tuning step | (stretch) | signal
    auto* header = new QHBoxLayout; header->setSpacing(12); header_ = header;
    freq_ = new FrequencyDisplay; freq_->setFixedHeight(64); freq_->setHz(7.1e6);
    mode_chip_ = new QLabel; mode_chip_->setObjectName("muted"); source_label_ = new QLabel; source_label_->setObjectName("muted"); source_label_->setAlignment(Qt::AlignRight);
    {   // the master switch: which kind of operating the rest of the screen is laid out for
        auto* col = new QVBoxLayout; col->setSpacing(6); mode_switch_col_ = col; grp_mode_ = group(this, true); const char* names[3] = {"Voice", "CW", "Digi"};
        const char* tips[3] = {"Voice: USB, LSB, AM, FM. Shows the voice transmit controls; no decoder.", "CW: Morse. Shows the decoder (Morse).", "Digital: FT8, FT4, PSK31, RTTY. Shows the decoder and the message controls."};
        for (int i = 0; i < 3; ++i) { auto* bt = pill(names[i]); bt->setToolTip(tips[i]); grp_mode_->addButton(bt, i); col->addWidget(bt); if (i == 0) bt->setChecked(true); }
        header->addWidget(card("Mode", col), 0); }
    auto* freq_body = new QVBoxLayout; freq_body->setSpacing(4); freq_body->addWidget(freq_);
    auto* chips = new QHBoxLayout; chips->addWidget(mode_chip_); chips->addStretch(1); chips->addWidget(source_label_); freq_body->addLayout(chips);
    auto* steps_row = new QHBoxLayout; steps_row->setSpacing(6); steps_ = group(this, true);
    const std::pair<const char*, double> steps[] = {{"10 Hz", 10}, {"100 Hz", 100}, {"1 kHz", 1000}, {"10 kHz", 10000}};
    for (int i = 0; i < 4; ++i) { auto* bt = pill(steps[i].first); bt->setToolTip("Tuning step (click the spectrum to tune)"); steps_->addButton(bt, i); steps_row->addWidget(bt); if (i == 1) bt->setChecked(true); }
    freq_body->addLayout(steps_row);
    header->addWidget(card("Frequency (MHz)", freq_body), 0);
    // (no memories box any more: the saved list still loads, nothing shows it)
    mem_combo_ = new QComboBox(this); mem_combo_->setVisible(false);
    connect(mem_combo_, &QComboBox::activated, this, [this](int i) { recallMemory(i); });
    meter_ = new SMeter; meter_text_ = new QLabel("no signal"); meter_text_->setObjectName("muted");
    meter_text_->ensurePolished();                                                       // measure with the themed font, then reserve the width of the longest possible text so the card never resizes as the numbers change
    meter_text_->setWordWrap(true); meter_text_->setMinimumWidth(std::max(meter_->minimumWidth(), 280));              // two lines, fixed height: the card never resizes as the numbers change
    meter_text_->setFixedHeight(2 * meter_text_->fontMetrics().lineSpacing() + 2); meter_text_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* meter_body = new QVBoxLayout; meter_body->setSpacing(2); meter_body->addWidget(meter_); meter_body->addWidget(meter_text_);
    meters_grid_ = new QGridLayout; meters_grid_->setHorizontalSpacing(16); meters_grid_->setVerticalSpacing(2); meters_grid_->addLayout(meter_body, 0, 0);
    signal_card_ = card("Meters: signal (dBFS, not calibrated) and transmit power", meters_grid_);
    root->addLayout(header);

    // ---- display: spectrum over waterfall in one panel
    spectrum_ = new SpectrumWidget; waterfall_ = new WaterfallWidget;
    auto* disp = new QFrame; disp->setObjectName("card"); auto* dcol = new QVBoxLayout(disp); dcol->setContentsMargins(1, 1, 1, 1); dcol->setSpacing(0);
    dcol->addWidget(spectrum_, 2); dcol->addWidget(waterfall_, 3);
    disp->setMinimumHeight(280);                               // the spectrum and waterfall always keep room, whatever the cards below ask for
    root->addWidget(disp, 1);

    // ---- decoder: text from CW / PSK31 / RTTY
    { auto* body = new QVBoxLayout; body->setSpacing(6); auto* row = new QHBoxLayout; row->setSpacing(8);
      dec_mode_ = new QComboBox; dec_mode_->addItems({"Off", "CW (Morse)", "PSK31", "RTTY", "FT8", "FT4", "WSPR"}); dec_mode_->setToolTip("Decodes text from the received audio. Picking a decoder also sets the right receiver mode.");
      dec_tone_ = new QSpinBox; dec_tone_->setRange(200, 3000); dec_tone_->setSingleStep(10); dec_tone_->setValue(700); dec_tone_->setSuffix(" Hz"); dec_tone_->setToolTip("CW: tone pitch. PSK31: signal centre. RTTY: mark tone, the higher of the two (space is 170 Hz below it, as on the air). FT8 and FT4 need none: they decode 200-3000 Hz. Double-click a decoded FT8/FT4 line to copy the station into the Log tab.");
      auto* clear = new QPushButton("Clear"); dec_status_ = new QLabel("Decoder off"); dec_status_->setObjectName("muted");
      tx_btn_ = new QPushButton("TX lab  \u25B8"); tx_btn_->setCheckable(true); tx_btn_->setVisible(false); tx_btn_->setToolTip("Show or hide the transmit lab (offline: renders files, not connected to the radio). The radio's transmit controls are to the right of the decoder.");
      my_call_ = new QLineEdit; my_call_->setPlaceholderText("my call"); my_call_->setMaximumWidth(100);
      my_call_->setToolTip("Your callsign. FT8 then also decodes weak messages addressed to you (shown with \"a2\", as in WSJT-X). Empty: the Log tab's station callsign is used.");
      connect(my_call_, &QLineEdit::editingFinished, this, [this] { my_call_->setText(my_call_->text().trimmed().toUpper()); if (decoderIndex() == 4 || decoderIndex() == 6) rebuildDecoder(); });
      clear_qso_ = new QPushButton("Clear QSO"); clear_qso_->setVisible(false); clear_qso_->setToolTip("Empties the QSO pane (right) and forgets the station being worked there. Transmitting is not affected: Stop does that.");
      connect(clear_qso_, &QPushButton::clicked, this, [this] { clearQso(); });
      ext_box_ = new QCheckBox("External program");
      ext_box_->setToolTip("Another program (WSJT-X, JS8Call, fldigi ...) decodes, logs and spots: our decoder, auto sequence + log and PSK Reporter uploads are turned off "
                           "(and come back as they were when you untick this). Receive audio goes to \"Fireside RX audio\" (Audio tab).");
      row->addWidget(dec_mode_); row->addWidget(ext_box_); row->addWidget(dec_tone_); row->addWidget(my_call_); row->addWidget(clear); row->addWidget(clear_qso_); row->addWidget(dec_status_, 1); row->addWidget(tx_btn_);
      dec_text_ = new QPlainTextEdit; dec_text_->setReadOnly(true); dec_text_->setMaximumBlockCount(1000); dec_text_->setFixedHeight(84); dec_text_->setVisible(false); dec_text_->setPlaceholderText("Decoded text appears here");
      body->addLayout(row);
      qso_text_ = new QPlainTextEdit; qso_text_->setReadOnly(true); qso_text_->setMaximumBlockCount(400); qso_text_->setPlaceholderText("Your QSOs: what is sent to you, and what you send"); qso_text_->setVisible(false);
      connect(qso_text_->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int mx) { qso_text_->verticalScrollBar()->setValue(mx); });   // always follows the newest line
      qso_text_->setToolTip("Only what concerns you: lines addressed to your call, and your own transmissions (TX). Everything else on the band (other stations' calls, the station you are working talking to someone else) stays in the decoder pane. It always scrolls to the newest line. Double-click a line to reply.");
      {   // the queue: a title and up to four rows (more scroll), each [up] [x] line | tag info
        callers_ = new QFrame; callers_->setVisible(false); callers_->setObjectName("queueBox");
        auto* qv = new QVBoxLayout(callers_); qv->setContentsMargins(0, 0, 0, 0); qv->setSpacing(2);
        queue_title_ = new QLabel("QUEUE  (empty: stations calling you wait here)"); queue_title_->setStyleSheet("color:#8b93a7; font-size:8pt; font-weight:bold;");
        queue_title_->setToolTip("First come, first served: the top station is worked next when a QSO ends (auto mode). \u25B2 moves a station up one place, \u2715 removes it (not listed again for 10 minutes), "
                                 "double-click a station to reply now. During a QSO, a double-click on a decode puts that station at the top instead of breaking off the QSO; "
                                 "a station that called you and is then heard calling someone else leaves the queue.");
        auto* rows = new QWidget; rows->setObjectName("queueRows"); queue_rows_ = new QVBoxLayout(rows); queue_rows_->setContentsMargins(0, 0, 0, 0); queue_rows_->setSpacing(1); queue_rows_->addStretch(1);
        auto* sc = new QScrollArea; sc->setObjectName("queueScroll"); sc->setWidget(rows); sc->setWidgetResizable(true); sc->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        sc->setStyleSheet("#queueScroll { background: #0b0e14; border: 1px solid #2a3442; border-radius: 6px; } #queueRows { background: #0b0e14; }");   // the decoder box's dark background
        sc->viewport()->setStyleSheet("background: #0b0e14;");
        { const QFontMetrics fm(dec_text_->font()); sc->setFixedHeight(4 * (fm.lineSpacing() + 8) + 6); }
        qv->addWidget(queue_title_); qv->addWidget(sc);
      }
      dec_left_ = new QWidget; { auto* lv = new QVBoxLayout(dec_left_); lv->setContentsMargins(0, 0, 0, 0); lv->setSpacing(4); lv->addWidget(callers_); lv->addWidget(dec_text_, 1); }
      { auto* ct = new QTimer(this); ct->setInterval(1000); connect(ct, &QTimer::timeout, this, &ReceiverPanel::tickCallers); ct->start(); }
      { auto* sp = new QSplitter(Qt::Horizontal); dec_left_->setMinimumWidth(340); dec_left_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding); sp->addWidget(dec_left_); sp->addWidget(qso_text_); sp->setStretchFactor(0, 3); sp->setStretchFactor(1, 2); sp->setSizes({600, 400}); sp->setChildrenCollapsible(false); dec_split_ = sp; body->addWidget(sp, 1); } dec_card_ = theme::makeCard("Decoder", body, true); root->addWidget(dec_card_);   // expand: the list fills the card beside the transmit card
      dec_text_->viewport()->installEventFilter(this); qso_text_->viewport()->installEventFilter(this);
      dec_text_->viewport()->setCursor(Qt::ArrowCursor); qso_text_->viewport()->setCursor(Qt::ArrowCursor);   // a pointer, not a text cursor (user 2026-11-27)
      connect(dec_mode_, &QComboBox::activated, this, [this](int i) { setDecoderIndex(i); });
      connect(dec_tone_, &QSpinBox::editingFinished, this, [this] { rebuildDecoder(); applySettings(); });
      connect(clear, &QPushButton::clicked, this, [this] { clearDecoded(); }); }

    // ---- control deck
    auto* deck = new QHBoxLayout; deck_ = deck; deck->setSpacing(12); 
    { auto* g = new QGridLayout; g->setSpacing(6); bands_ = group(this, false);
      for (int i = 0; i < int(std::size(kBands)); ++i) { auto* b = pill(kBands[i].name); b->setToolTip(QString("%1 m: back where you left it (frequency, mode, filter, decoder)").arg(kBands[i].name)); bands_->addButton(b, i); g->addWidget(b, i / 6, i % 6); }
      preamp_ = pill("Preamp"); preamp_->setEnabled(false); preamp_->setToolTip("Receive preamp (more gain for weak signals; can overload on strong ones). Usable once the radio is started; off at every start-up and shutdown. Receive only.");
      g->addWidget(preamp_, int((std::size(kBands) + 5) / 6), 0, 1, 2); connect(preamp_, &QPushButton::clicked, this, [this](bool on) { emit preampRequested(on); });
      band_card_ = card("Band (m)", g); }
    auto* filter_col = new QVBoxLayout; filter_col->setSpacing(6);                    // the passband controls: they live in the Noise card, not the mode tile
    { modes_ = group(this, true); for (int i = 0; i < 5; ++i) { auto* b = pill(kModeNames[i]); modes_->addButton(b, i); if (i == 0) b->setChecked(true); }
      mode_pages_ = new QStackedWidget;                                                  // three pages, one per master-switch group; the buttons live on exactly one
      { auto* pg = new QWidget; auto* g = new QGridLayout(pg); g->setContentsMargins(0, 0, 0, 0); g->setSpacing(6);       // Voice: USB LSB AM FM
        g->addWidget(modes_->button(0), 0, 0); g->addWidget(modes_->button(1), 0, 1); g->addWidget(modes_->button(2), 1, 0); g->addWidget(modes_->button(4), 1, 1);
        auto_sb_ = new QCheckBox("Auto LSB / USB"); auto_sb_->setChecked(true); auto_sb_->setToolTip("Entering a band in USB or LSB picks its sideband: LSB on 160, 80 and 40 m, USB on 60 m and above");
        g->addWidget(auto_sb_, 2, 0, 1, 2); mode_pages_->addWidget(pg); }
      { auto* pg = new QWidget; auto* g = new QGridLayout(pg); g->setContentsMargins(0, 0, 0, 0); g->setSpacing(6);       // CW
        g->addWidget(modes_->button(3), 0, 0); g->setColumnStretch(1, 1); mode_pages_->addWidget(pg); }
      { auto* pg = new QWidget; auto* g = new QGridLayout(pg); g->setContentsMargins(0, 0, 0, 0); g->setSpacing(6);       // Digital: not modes of their own: USB + the decoder (FT8 / FT4 also the band's FT frequency)
        ft8_btn_ = pill("FT8"); ft4_btn_ = pill("FT4"); psk_btn_ = pill("PSK31"); rtty_btn_ = pill("RTTY");
        ft8_btn_->setToolTip("FT8: the band's FT8 frequency, USB, the wide filter and the FT8 decoder"); ft4_btn_->setToolTip("FT4: the band's FT4 frequency, USB, the wide filter and the FT4 decoder");
        psk_btn_->setToolTip("PSK31: USB and the PSK31 decoder (the dial stays where it is)"); rtty_btn_->setToolTip("RTTY: USB and the RTTY decoder (the dial stays where it is)");
        wspr_btn_ = pill("WSPR"); wspr_btn_->setToolTip("WSPR: the band's WSPR frequency, USB, the wide filter and WSJT-X's wsprd decoder (two-minute slots); the beacon is on the Transmit card");
        g->addWidget(ft8_btn_, 0, 0); g->addWidget(ft4_btn_, 0, 1); g->addWidget(psk_btn_, 1, 0); g->addWidget(rtty_btn_, 1, 1); g->addWidget(wspr_btn_, 2, 0); mode_pages_->addWidget(pg); }
      connect(wspr_btn_, &QPushButton::clicked, this, [this] { selectWspr(); });
      connect(ft8_btn_, &QPushButton::clicked, this, [this] { selectFt(false); }); connect(ft4_btn_, &QPushButton::clicked, this, [this] { selectFt(true); });
      connect(psk_btn_, &QPushButton::clicked, this, [this] { setDecoderIndex(2); }); connect(rtty_btn_, &QPushButton::clicked, this, [this] { setDecoderIndex(3); });
      filter_row_ = new QHBoxLayout; filter_row_->setSpacing(6); filters_ = group(this, true); rebuildFilterButtons();
      auto* col = new QVBoxLayout; col->setSpacing(8); col->addWidget(mode_pages_); auto* fl = new QLabel("Passband width"); fl->setObjectName("muted"); filter_col->addWidget(fl); filter_col->addLayout(filter_row_);
      { auto* cr = new QHBoxLayout; cr->setSpacing(6); cut_lo_label_ = new QLabel("Low"); cut_hi_label_ = new QLabel("High"); cut_lo_label_->setObjectName("muted"); cut_hi_label_->setObjectName("muted");
        cut_lo_ = new QSpinBox; cut_lo_->setRange(0, 5000); cut_lo_->setSingleStep(10); cut_lo_->setSuffix(" Hz"); cut_hi_ = new QSpinBox; cut_hi_->setRange(100, 8000); cut_hi_->setSingleStep(10); cut_hi_->setSuffix(" Hz");
        cut_lo_->setToolTip("Low cut of the passband (audio Hz). Also: drag the edges of the shaded passband on the spectrum"); cut_hi_->setToolTip("High cut of the passband (audio Hz). For AM, CW and FM this is the total width");
        shape_box_ = new QComboBox; shape_box_->addItems({"Soft", "Normal", "Sharp"}); shape_box_->setCurrentIndex(1); shape_box_->setToolTip("Filter skirt: Sharp rejects neighbours better but rings more (best for CW and data); Soft sounds more natural on voice");
        cr->addWidget(cut_lo_label_); cr->addWidget(cut_lo_); cr->addWidget(cut_hi_label_); cr->addWidget(cut_hi_); cr->addWidget(shape_box_); filter_col->addLayout(cr); syncFilterEdits();
        am_det_ = new QComboBox; am_det_->addItems({"Sync AM", "Sync AM: lower sideband", "Sync AM: upper sideband", "Envelope (plain)"});
        am_det_->setToolTip("AM detector. Sync AM locks to the carrier: no distortion when the carrier fades (medium wave at night), a little quieter on weak stations. "
                            "One sideband: drops a whistle or a neighbour on the other side. Envelope: the plain detector (use it if a station has no steady carrier).");
        am_det_->setVisible(false); filter_col->addWidget(am_det_);
        connect(am_det_, &QComboBox::activated, this, [this](int) { applySettings(); });
        buildFmControls(filter_col); }
      col->addStretch(1);
      mode_card_ = card("Voice modes", col); header->addWidget(mode_card_, 0); header->addWidget(band_card_, 0); header->addWidget(signal_card_, 1); }       // after Frequency: the modes of the chosen group, then the bands
    { auto* col = new QVBoxLayout; col->setSpacing(8); form_slot_ = col; form_card_ = card("Log QSO", col); form_card_->setVisible(false); deck->addWidget(form_card_, 6); }       // the large log form: shown only while a contact is being logged (it sits where Band and Mode are)
    { auto* col = new QVBoxLayout; col->setSpacing(8); log_slot_ = col; log_card_ = theme::makeCard("Lookup", col, true); log_card_->setVisible(false); log_card_->setMaximumWidth(640); deck->addWidget(log_card_, 3); }       // filled by setLookupWidget (callsign, Enter, who is it)
    { auto* col = new QVBoxLayout; col->setSpacing(8); auto* agc_row = new QHBoxLayout; agc_row->setSpacing(6); agcs_ = group(this, true);
      for (int i = 0; i < 4; ++i) { auto* b = pill(kAgcNames[i]); agcs_->addButton(b, i); agc_row->addWidget(b); if (i == 2) b->setChecked(true); }
      auto* vol = new QHBoxLayout; volume_ = new QSlider(Qt::Horizontal); volume_->setRange(0, 100); volume_->setValue(60); volume_->setMinimumWidth(110); mute_ = pill("Mute");
      vol->addWidget(new QLabel(audioOut_->available() ? "Vol" : "Vol (no output)")); vol->addWidget(volume_, 1); vol->addWidget(mute_);
      auto* agc_label = new QLabel("AGC"); agc_label->setObjectName("muted"); col->addWidget(agc_label); col->addLayout(agc_row);
      auto* gr = new QHBoxLayout; gain_ = new QSlider(Qt::Horizontal); gain_->setRange(0, 100); gain_->setValue(60); gain_->setMinimumWidth(110); gain_label_ = new QLabel("Gain 60 dB"); gain_label_->setMinimumWidth(66);
      gain_->setToolTip("Fixed gain, used only while the AGC is Off (the radio's signal is very quiet before the AGC: about 60 dB makes it audible). Turn it down for strong stations; very loud peaks are soft-limited.");
      gr->addWidget(gain_label_); gr->addWidget(gain_, 1); col->addLayout(gr); col->addLayout(vol);
      connect(gain_, &QSlider::valueChanged, this, [this](int v) { gain_label_->setText(QString("Gain %1 dB").arg(v)); settingsChanged(); });
      connect(agcs_, &QButtonGroup::idToggled, this, [this](int, bool) { gain_->setEnabled(agcIndex() == 0); gain_label_->setEnabled(agcIndex() == 0); }); gain_->setEnabled(false); gain_label_->setEnabled(false);
      auto* outrow = new QHBoxLayout; outrow->setSpacing(6); out_target_ = new QComboBox; out_target_->addItems({"Output: PC sound", "Output: Radio jacks"}); out_pair_ = new QComboBox; out_pair_->addItems({"channels: not chosen", "ch 0/1", "ch 2/3", "ch 4/5", "ch 6/7"});
      out_target_->setToolTip("PC sound: the computer's speakers. Radio jacks: the radio's headphone / powered-speaker output via FireWire (needs a Live capture open and a channel pair chosen after the output test: tools/firesideplay)");
      out_pair_->setToolTip("Which pair of the radio's 8 output channels carries the audio. Leave 'not chosen' until the output-channel test has been run on the real radio."); out_pair_->setEnabled(false);
      setRadioOutputsAvailable(false);               // until a live capture names the radio's card
      // One box with the outputs by name (the radio's channels are known: ch4/5 powered speakers, ch2/3 headphones, ch6 RCA line out);
      // the target / pair boxes stay behind it (hidden) for the existing logic.
      out_dest_ = new QComboBox; out_dest_->addItems({"PC speakers", "Radio: powered speakers", "Radio: headphones", "Radio: line out (RCA)"});
      out_dest_->setToolTip("Where the receive audio plays. The radio's outputs need the direct stream (48 / 96 / 192 in the Source card).");
      out_target_->setVisible(false); out_pair_->setVisible(false);
      outrow->addWidget(out_dest_, 1); outrow->addWidget(out_target_); outrow->addWidget(out_pair_); col->addLayout(outrow);
      connect(out_dest_, &QComboBox::activated, this, [this](int i) {
          static const int pair[] = {0, 3, 2, 4};                                            // out_pair_ index: 3 = ch 4/5, 2 = ch 2/3, 4 = ch 6/7
          { QSignalBlocker a(out_target_), b(out_pair_); out_target_->setCurrentIndex(i == 0 ? 0 : 1); out_pair_->setCurrentIndex(pair[std::clamp(i, 0, 3)]); }
          outputControlsChanged(); syncOutDest(); });
      setRadioOutputsAvailable(false);
      deck->addWidget(card("Audio", col), 2); }
    { auto* col = new QVBoxLayout; col->setSpacing(8); auto* r1 = new QHBoxLayout; r1->setSpacing(6);
      nb_ = pill("NB"); nr_ = pill("NR"); anf_ = pill("Auto notch"); nb_->setToolTip("Impulse noise blanker"); nr_->setToolTip("Adaptive line filter: keeps steady tones (CW, whistles) and drops the rest. For speech use NR2"); anf_->setToolTip("Removes steady tones automatically");
      nr2_ = pill("NR2"); nr2_->setVisible(fireside::dsp::spectral_nr_available());
      nr2_->setToolTip("Spectral noise reduction for voice (WDSP NR2): learns the noise and turns it down. About 0.1 s of delay. Not used for CW, and not while PSK31 / RTTY / FT8 / FT4 are decoding (they work better on the untouched audio)");
      r1->addWidget(nb_); r1->addWidget(nr_); r1->addWidget(nr2_); r1->addWidget(anf_);
      auto* r2 = new QHBoxLayout; r2->setSpacing(6); notch_ = new QSpinBox; notch_->setRange(0, 5000); notch_->setSingleStep(50); notch_->setSpecialValueText("Notch off"); notch_->setSuffix(" Hz");
      squelch_ = new QSpinBox; squelch_->setRange(-120, 0); squelch_->setValue(-120); squelch_->setSpecialValueText("Squelch off"); squelch_->setSuffix(" dB"); r2->addWidget(notch_); r2->addWidget(squelch_);
      col->addLayout(r1); col->addLayout(r2); col->addLayout(filter_col); deck->addWidget(card("Noise and filter", col), 2); }
    { auto* col = new QVBoxLayout; col->setSpacing(8); auto* r1 = new QHBoxLayout; r1->setSpacing(6);
      auto* open = new QPushButton("Open file..."); auto* demo = new QPushButton("Demo");
      auto* rr = new QHBoxLayout; rr->setSpacing(4); auto* grp = new QButtonGroup(this); grp->setExclusive(true);
      static const unsigned kRates[3] = {48000, 96000, 192000}; static const char* const kNames[3] = {"48", "96", "192"};
      for (int i = 0; i < 3; ++i) {
          rate_btn_[i] = new QPushButton(kNames[i]); rate_btn_[i]->setCheckable(true); rate_btn_[i]->setCursor(Qt::PointingHandCursor); grp->addButton(rate_btn_[i]); rr->addWidget(rate_btn_[i]);
          rate_btn_[i]->setToolTip(QString("%1 kHz of band. Starts the radio (receive only) and the live stream at this rate; if the radio is already running, switches just the stream (about a second). The audio is 48 kHz at every rate.").arg(kNames[i]));
          connect(rate_btn_[i], &QPushButton::clicked, this, [this, i] { if (kRates[i] == direct_rate_) { syncRateButtons(direct_rate_); return; } emit rateRequested(kRates[i]); syncRateButtons(direct_rate_); });   // (the button shows the stream that really runs)
      }
      // no Stop radio button here (user 2026-11-28): the rates just switch; the Device tab's Stop radio, or closing Fireside, puts the radio in standby
      r1->addWidget(open); r1->addWidget(demo);
      play_ = new QPushButton("Play"); play_->setObjectName("primary"); play_->setCheckable(true); play_->setCursor(Qt::PointingHandCursor);
      { auto* cap = new QLabel("Radio: kHz of band"); cap->setObjectName("dim"); col->addWidget(cap); }
      auto* rr2 = new QHBoxLayout; rr2->setSpacing(4);
      grab_btn_ = new QPushButton("Grab 30 s"); rec_btn_ = new QPushButton("Rec"); auto* recopt = new QPushButton("..."); recopt->setMaximumWidth(34); recopt->setToolTip("Recording options: replay length, what to save, folder, files saved this session");
      grab_btn_->setToolTip("Saves the last 30 s of the live stream and of the audio you heard (Ctrl+G). The receiver keeps the last minute in memory all the time, so you can press this AFTER you hear something."); grab_btn_->setEnabled(false);
      rec_btn_->setToolTip("Start / stop a recording of the live stream, beginning with the last few seconds (Ctrl+Shift+R)"); rec_btn_->setEnabled(false);
      rr2->addWidget(grab_btn_, 1); rr2->addWidget(rec_btn_, 1); rr2->addWidget(recopt);
      connect(recopt, &QPushButton::clicked, this, [this] { if (rec_) rec_->showOptions(this); });
      col->addLayout(rr); col->addLayout(r1); col->addWidget(play_); col->addLayout(rr2);
      connect(open, &QPushButton::clicked, this, &ReceiverPanel::openFile); connect(demo, &QPushButton::clicked, this, [this] { loadDemo(); });
      deck->addWidget(card("Source", col), 1); }
    { auto* col = new QVBoxLayout; col->setSpacing(8); mirror_ = pill("Mirror I/Q"); mirror_->setToolTip("The radio's I/Q is mirrored: ON for the live radio (comes up on by itself). Recordings keep their own setting: tick it for files captured from the radio");
      if_ = new QComboBox; if_->addItems({"IF offset 9 kHz", "IF offset 0"}); if_->setToolTip("9 kHz = the radio's DDS sits 9 kHz below the dial (what the start-up does, as PowerSDR; keeps carriers out of the radio's 0 Hz hole). 0 = the I/Q is centred on the dial (most recordings)");
      loop_ = new QCheckBox("Loop file"); loop_->setChecked(true);
      loop_->setToolTip("Recordings played with Play start again at the end");
      /* not on the Radio screen: MainWindow puts this row on the Device tab (takeAdvancedWidget) */
      advanced_ = new QWidget; auto* ar = new QHBoxLayout(advanced_); ar->setContentsMargins(0, 0, 0, 0); ar->setSpacing(12);
      ar->addWidget(mirror_); ar->addWidget(if_); ar->addWidget(loop_); ar->addStretch(1); delete col; }
    { auto* col = new QVBoxLayout; col->setSpacing(8); palette_ = new QComboBox; for (int i = 0; i < display::kPaletteCount; ++i) palette_->addItem(display::palette_name(static_cast<display::Palette>(i)));
      floor_ = new QSlider(Qt::Horizontal); floor_->setRange(-30, 30); floor_->setValue(0); floor_->setToolTip("Waterfall floor: right = darker noise, left = brighter");
      auto* l1 = new QLabel("Colours"); l1->setObjectName("muted"); auto* l2 = new QLabel("Floor"); l2->setObjectName("muted");
      settle_ = new QComboBox; settle_->addItems({"Quick (5 s)", "Normal (20 s)", "Slow (60 s)"}); settle_->setCurrentIndex(1); settle_->setToolTip("How slowly the spectrum's dB axis narrows again after a strong signal has gone. It always widens quickly. Slower = steadier.");
      auto* l3 = new QLabel("Axis settling"); l3->setObjectName("muted");
      auto* wg = new QGridLayout; wg->setHorizontalSpacing(10); wg->setVerticalSpacing(4);      // two columns: half the height of one
      wg->addWidget(l1, 0, 0); wg->addWidget(palette_, 1, 0); wg->addWidget(l2, 2, 0); wg->addWidget(floor_, 3, 0);
      wg->addWidget(l3, 0, 1); wg->addWidget(settle_, 1, 1);
      auto* l4 = new QLabel("Spectrum averaging"); l4->setObjectName("dim");
      avg_ = new QComboBox; avg_->addItems({"Off", "Light", "Medium", "Heavy"}); avg_->setCurrentIndex(1); avg_->setToolTip("Smooths the spectrum trace from frame to frame (the waterfall is not smoothed). Light is a little steadier without hiding fast signals.");
      wg->addWidget(l4, 2, 1); wg->addWidget(avg_, 3, 1);
      auto* l5 = new QLabel("Zoom"); l5->setObjectName("dim"); auto* zr = new QHBoxLayout; zr->setSpacing(4);
      zoom_out_ = new QPushButton("-"); zoom_in_ = new QPushButton("+"); zoom_full_ = new QPushButton("Full");
      zoom_out_->setToolTip("Zoom out (Ctrl+-). Ctrl+wheel over the spectrum or waterfall also zooms, about the mouse."); zoom_in_->setToolTip("Zoom in (Ctrl+=)"); zoom_full_->setToolTip("Whole span (Ctrl+0)");
      for (auto* b : {zoom_out_, zoom_in_, zoom_full_}) { b->setAutoDefault(false); b->setFocusPolicy(Qt::NoFocus); zr->addWidget(b); }
      wg->addWidget(l5, 4, 0); wg->addLayout(zr, 5, 0);
      bandplan_ = new QCheckBox("Band plan"); bandplan_->setChecked(true); bandplan_->setToolTip("Strip along the top of the spectrum: RSGB 2026 usage (CW, narrow/digital, all modes, beacons, FM) inside the Ofcom Full-licence allocations (OFW611, Feb 2024); red = inside a band but NOT a UK allocation (60 m gaps); FT8/FT4 dial frequencies. Hover for details. A guide only."); wg->addWidget(bandplan_, 5, 1); col->addLayout(wg); col->addStretch(1); deck->addWidget(card("Waterfall", col), 1); }
    root->addLayout(deck);

    status_ = new QLabel; status_->setObjectName("status"); status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    {   // the bottom line changes ~30 times a second: a fixed-pitch font and fixed-width number fields keep every character where it is, and the label
        // never asks the window for more room (a long line is clipped; the tooltip has all of it)
        QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont); f.setPointSizeF(status_->font().pointSizeF() > 0 ? status_->font().pointSizeF() : 9.0); status_->setFont(f);
        status_->setTextFormat(Qt::PlainText); status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); status_->setMinimumWidth(0);
        status_->setFixedHeight(QFontMetrics(f).height() + 8);
    }
    root->addWidget(status_);

    ui_ = new QTimer(this); ui_->setInterval(30); connect(ui_, &QTimer::timeout, this, &ReceiverPanel::refresh);
    connect(play_, &QPushButton::toggled, this, [this](bool on) { setPlaying(on); });
    connect(bands_, &QButtonGroup::idClicked, this, [this](int i) { recallBand(i); });
    connect(modes_, &QButtonGroup::idToggled, this, [this](int, bool on) { if (!on) return; filter_width_ = dsp::default_filter_width(static_cast<dsp::Mode>(modeIndex())); if (modeIndex() == 4 && fm_dev_ && fm_dev_->currentIndex() == 0) filter_width_ = 12000; filter_low_ = 150; rebuildFilterButtons(); syncGroup(); settingsChanged(); emit modeChanged(modeIndex()); });
    connect(grp_mode_, &QButtonGroup::idClicked, this, [this](int g) { chooseGroup(g); });
    connect(cut_lo_, &QSpinBox::editingFinished, this, [this] { cutsEdited(); }); connect(cut_hi_, &QSpinBox::editingFinished, this, [this] { cutsEdited(); });
    connect(shape_box_, &QComboBox::activated, this, [this](int i) { setFilterShape(i); });
    connect(spectrum_, &SpectrumWidget::passbandEdgeDragged, this, [this](double lo, double hi) {                 // dragging an edge on the spectrum
        switch (static_cast<dsp::Mode>(modeIndex())) {
            case dsp::Mode::USB: setPassbandCuts(lo, hi); break; case dsp::Mode::LSB: setPassbandCuts(-hi, -lo); break;
            default: setPassbandCuts(0, 2 * std::max(std::fabs(lo), std::fabs(hi))); }
    });
    connect(agcs_, &QButtonGroup::idToggled, this, [this](int, bool on) { if (on) settingsChanged(); });
    connect(steps_, &QButtonGroup::idToggled, this, [this, steps](int i, bool on) { if (on) step_hz_ = steps[i].second; });
    for (auto* b : {nb_, nr_, nr2_, anf_, mirror_.data()}) connect(b, &QPushButton::toggled, this, &ReceiverPanel::settingsChanged);
    connect(mirror_, &QPushButton::toggled, this, [this](bool on) { (source_live_ ? mirror_live_ : mirror_file_) = on; });   // the button belongs to the current kind of source
    connect(if_, &QComboBox::currentIndexChanged, this, &ReceiverPanel::settingsChanged);
    connect(squelch_, &QSpinBox::valueChanged, this, &ReceiverPanel::settingsChanged);
    connect(notch_, &QSpinBox::valueChanged, this, [this](int hz) {                                              // the box is notch number 1; 0 removes it
        if (hz == 0) { if (!notch_list_.empty()) notch_list_.erase(notch_list_.begin()); } else if (notch_list_.empty()) notch_list_.push_back(hz); else notch_list_[0] = hz;
        syncNotchBox(); settingsChanged(); });
    connect(spectrum_, &SpectrumWidget::notchToggled, this, [this](double off) {                                    // Alt+click on the spectrum
        const double f = std::round(std::clamp(rfToAudio(off), 100.0, 5000.0) / 10.0) * 10.0; int near = -1;
        for (size_t i = 0; i < notch_list_.size(); ++i) if (std::fabs(notch_list_[i] - f) < 60) near = int(i);
        if (near >= 0) notch_list_.erase(notch_list_.begin() + near); else if (notch_list_.size() < 4) notch_list_.push_back(f);
        syncNotchBox(); settingsChanged(); });
    connect(spectrum_, &SpectrumWidget::notchMoved, this, [this](int i, double off) { if (i >= 0 && size_t(i) < notch_list_.size()) { notch_list_[size_t(i)] = std::round(std::clamp(rfToAudio(off), 100.0, 5000.0) / 10.0) * 10.0; syncNotchBox(); settingsChanged(); } });
    connect(spectrum_, &SpectrumWidget::passbandShifted, this, [this](double d) {                                  // Ctrl+drag: slide the passband
        if (modeIndex() > 1) return; const double s = modeIndex() == 0 ? d : -d; if (cutLow() + s < 0) return; shift_acc_ += s;
        if (std::fabs(shift_acc_) >= 10) { const double step = std::round(shift_acc_ / 10) * 10; shift_acc_ -= step; setPassbandCuts(cutLow() + step, cutHigh() + step); } });
    connect(freq_, &FrequencyDisplay::valueChanged, this, [this](double hz) {
        spectrum_->setDial(hz); if (tune_sw_ && !restoring_) applySettings();                     // retune within the recording
        int idx = -1; for (int i = 0; i < int(std::size(kBands)); ++i) if (hz >= kBands[i].lo && hz <= kBands[i].hi) idx = i;           // highlight the band the dial is in
        for (int i = 0; i < int(std::size(kBands)); ++i) bands_->button(i)->setChecked(i == idx);
        if (idx != last_band_idx_) {                                                      // a new band: its sideband (voice modes only; never under an FT8 / FT4 decoder)
            const bool first = last_band_idx_ == -2; last_band_idx_ = idx;
            if (!first) { clearDecoded(); clearQso(); }                               // another band: the decodes and the QSO pane start empty (user 2026-11-27)
            if (!first && idx >= 0 && !restoring_ && !recalling_ && auto_sb_ && auto_sb_->isChecked() && (modeIndex() == 0 || modeIndex() == 1) && decoderIndex() < 4) {
                const QString n = kBands[idx].name; const int want = (n == "160" || n == "80" || n == "40") ? 1 : 0;
                if (modeIndex() != want) setModeIndex(want);
            }
        }
        noteBand(); heard_ftx_.clear(); emit dialChanged(hz); });   // stations heard on another dial are not where they are now
    connect(bandplan_, &QCheckBox::toggled, this, [this](bool on) { spectrum_->setBandPlan(on); });
    connect(avg_, &QComboBox::currentIndexChanged, this, [this](int i) { static const float a[] = {1.f, 0.5f, 0.3f, 0.15f}; spectrum_->setAveraging(a[std::clamp(i, 0, 3)]); });
    spectrum_->setAveraging(0.5f);
    rec_ = new RecordController(this);                                                                     // replay buffer + recording: taps the engine
    connect(grab_btn_, &QPushButton::clicked, rec_, &RecordController::grab); connect(rec_btn_, &QPushButton::clicked, rec_, &RecordController::toggleRecord);
    connect(rec_, &RecordController::message, this, &ReceiverPanel::showNote); connect(rec_, &RecordController::stateChanged, this, [this] { updateRecordButtons(); });
    { auto* g = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_G), this); g->setContext(Qt::WidgetWithChildrenShortcut); connect(g, &QShortcut::activated, rec_, &RecordController::grab);
      auto* r = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), this); r->setContext(Qt::WidgetWithChildrenShortcut); connect(r, &QShortcut::activated, rec_, &RecordController::toggleRecord); }
    updateRecordButtons();
    connect(zoom_in_, &QPushButton::clicked, this, [this] { zoomView(+1, (spectrum_->viewLo() + spectrum_->viewHi()) / 2); });          // buttons / keys zoom about the centre of the view
    connect(zoom_out_, &QPushButton::clicked, this, [this] { zoomView(-1, (spectrum_->viewLo() + spectrum_->viewHi()) / 2); });
    connect(zoom_full_, &QPushButton::clicked, this, [this] { setView(0, 1); });
    for (auto [seq, fn] : std::initializer_list<std::pair<QKeySequence, std::function<void()>>>{
             {QKeySequence(Qt::CTRL | Qt::Key_Equal), [this] { zoom_in_->click(); }}, {QKeySequence(Qt::CTRL | Qt::Key_Plus), [this] { zoom_in_->click(); }},
             {QKeySequence(Qt::CTRL | Qt::Key_Minus), [this] { zoom_out_->click(); }}, {QKeySequence(Qt::CTRL | Qt::Key_0), [this] { zoom_full_->click(); }}}) {
        auto* sc = new QShortcut(seq, this); sc->setContext(Qt::WidgetWithChildrenShortcut); connect(sc, &QShortcut::activated, this, fn);
    }
    connect(spectrum_, &SpectrumWidget::zoomRequested, this, [this](int n, double a) { zoomView(n, a); });
    connect(waterfall_, &WaterfallWidget::zoomRequested, this, [this](int n, double a) { zoomView(n, a); });
    connect(settle_, &QComboBox::currentIndexChanged, this, [this](int i) { setAxisSettling(i); });
    connect(palette_, &QComboBox::currentIndexChanged, this, [this](int i) { waterfall_->setPalette(static_cast<display::Palette>(i)); });
    connect(floor_, &QSlider::valueChanged, this, [this](int v) { waterfall_->setFloorOffsetDb(float(v)); });
    connect(spectrum_, &SpectrumWidget::wheelSteps, this, [this](int n) { stepDial(n); });                   // mouse wheel over the spectrum or waterfall tunes by the step
    connect(waterfall_, &WaterfallWidget::clickedAt, this, [this](double f) { setDialHz(std::round((dialHz() + (f - 0.5) * spectrum_->spanHz()) / 10.0) * 10.0); });   // click-to-tune on the waterfall: where you click (to 10 Hz), as on the spectrum
    connect(waterfall_, &WaterfallWidget::wheelSteps, this, [this](int n) { stepDial(n); });
    connect(out_target_, &QComboBox::activated, this, [this](int) { outputControlsChanged(); });
    connect(out_pair_, &QComboBox::activated, this, [this](int) { outputControlsChanged(); });
    connect(loop_, &QCheckBox::toggled, this, [this](bool on) { engine_.with_source([on](IqSource* s) { if (s) s->set_loop(on); }); });
    connect(spectrum_, &SpectrumWidget::offsetClicked, this, [this](double off) { setDialHz(std::round((dialHz() + off) / 10.0) * 10.0); });   // click-to-tune: where you click (to 10 Hz); the step is for the knob / wheel / buttons
    spectrum_->setDial(freq_->hz()); bands_->button(3)->setChecked(true);
    ui_->start();
    auto sc = [this](const QKeySequence& k, std::function<void()> f) { auto* s = new QShortcut(k, this); s->setContext(Qt::WindowShortcut); connect(s, &QShortcut::activated, this, std::move(f)); };
    sc(Qt::Key_Right, [this] { stepDial(+1); }); sc(Qt::Key_Left, [this] { stepDial(-1); });
    sc(Qt::SHIFT | Qt::Key_Right, [this] { stepDial(+10); }); sc(Qt::SHIFT | Qt::Key_Left, [this] { stepDial(-10); });
    sc(Qt::Key_PageUp, [this] { nextBand(+1); }); sc(Qt::Key_PageDown, [this] { nextBand(-1); });
    sc(Qt::Key_M, [this] { mute_->toggle(); }); sc(Qt::Key_Space, [this] { play_->toggle(); });
    note_ = "Open an I/Q WAV (RX1 I/Q = channels 1 and 2), press Demo, or try Live.   Keys: arrows tune, PgUp/PgDn change band, M mute, Space play/pause."; updateStatus();
    loadSettings();
}
ReceiverPanel::~ReceiverPanel() {
    saveSettings(); engine_.stop();
    delete rec_; rec_ = nullptr;      // its destructor unhooks the engine's taps: it must run here, while engine_ still exists (as a child it would be deleted after the members)
}

void ReceiverPanel::rebuildFilterButtons() {
    // Five buttons are created once; a mode change only relabels and hides them (deleting and recreating widgets left the card half-painted).
    const auto widths = dsp::filter_widths(static_cast<dsp::Mode>(modeIndex()));
    if (filters_->buttons().isEmpty()) {
        for (int i = 0; i < 5; ++i) {
            auto* b = pill(""); filters_->addButton(b, i); filter_row_->addWidget(b);
            connect(b, &QPushButton::clicked, this, [this, i] { const auto w = dsp::filter_widths(static_cast<dsp::Mode>(modeIndex())); if (i < int(w.size())) { filter_width_ = w[size_t(i)]; applySettings(); updateStatus(); } });
        }
    }
    for (int i = 0; i < 5; ++i) {
        auto* b = filters_->button(i); const bool used = i < int(widths.size());
        b->setVisible(used); if (used) { b->setText(width_text(widths[size_t(i)])); b->setChecked(widths[size_t(i)] == filter_width_); } else b->setChecked(false);
    }
    syncFilterEdits();
}
int ReceiverPanel::modeIndex() const { return std::max(0, modes_->checkedId()); }
void ReceiverPanel::setModeIndex(int i) { if (auto* b = modes_->button(i)) b->setChecked(true); }
int ReceiverPanel::agcIndex() const { return std::max(0, agcs_->checkedId()); }
void ReceiverPanel::setAgcIndex(int i) { if (auto* b = agcs_->button(i)) b->setChecked(true); }
double ReceiverPanel::rfToAudio(double off) const {        // spectrum offset from the dial -> audio frequency the notch must sit at
    switch (modeIndex()) { case 0: return off; case 1: return -off; case 3: return off + double(dec_tone_->value() > 0 && decoderIndex() == 1 ? dec_tone_->value() : 700); default: return off; }
}
double ReceiverPanel::audioToRf(double f) const {
    switch (modeIndex()) { case 0: return f; case 1: return -f; case 3: return f - double(dec_tone_->value() > 0 && decoderIndex() == 1 ? dec_tone_->value() : 700); default: return f; }
}
void ReceiverPanel::updateSpectrumNotches() {
    std::vector<double> rf; if (modeIndex() == 0 || modeIndex() == 1 || modeIndex() == 3) for (double f : notch_list_) rf.push_back(audioToRf(f));      // AM and FM have no clear mapping: no markers
    spectrum_->setNotchOffsets(rf);
}
void ReceiverPanel::syncNotchBox() { QSignalBlocker b(notch_); notch_->setValue(notch_list_.empty() ? 0 : int(notch_list_[0])); updateSpectrumNotches(); }
void ReceiverPanel::setNotchesHz(const std::vector<double>& hz) { notch_list_.clear(); for (double f : hz) if (f > 0 && notch_list_.size() < 4) notch_list_.push_back(f); syncNotchBox(); settingsChanged(); }
void ReceiverPanel::syncFilterEdits() {                                      // the Low/High boxes follow the passband (SSB: two cuts; AM/CW/FM: one width)
    if (!cut_lo_) return; const bool ssb = modeIndex() == 0 || modeIndex() == 1; QSignalBlocker a(cut_lo_), b(cut_hi_);
    cut_lo_->setVisible(ssb); cut_lo_label_->setVisible(ssb); cut_hi_label_->setText(ssb ? "High" : "Width");
    cut_hi_->setRange(ssb ? 100 : 50, ssb ? 8000 : 20000); cut_lo_->setValue(int(std::lround(filter_low_))); cut_hi_->setValue(int(std::lround(ssb ? filter_low_ + filter_width_ : filter_width_)));
}
void ReceiverPanel::cutsEdited() { const bool ssb = modeIndex() == 0 || modeIndex() == 1; setPassbandCuts(ssb ? cut_lo_->value() : 0, cut_hi_->value()); }
void ReceiverPanel::setPassbandCuts(double low, double high) {
    if (modeIndex() == 0 || modeIndex() == 1) { low = std::clamp(low, 0.0, 5000.0); high = std::clamp(high, low + 100.0, 8000.0); filter_low_ = low; filter_width_ = high - low; }
    else filter_width_ = std::clamp(high, 50.0, 20000.0);
    rebuildFilterButtons(); applySettings(); updateStatus();
}
double ReceiverPanel::cutLow() const { return filter_low_; }
double ReceiverPanel::cutHigh() const { return (modeIndex() == 0 || modeIndex() == 1) ? filter_low_ + filter_width_ : filter_width_; }
void ReceiverPanel::setFilterShape(int i) { filter_shape_ = std::clamp(i, 0, 2); { QSignalBlocker b(shape_box_); shape_box_->setCurrentIndex(filter_shape_); } applySettings(); }
void ReceiverPanel::setFilterWidth(double hz) { filter_width_ = hz; rebuildFilterButtons(); applySettings(); updateStatus(); }
QAbstractButton* ReceiverPanel::mirrorBox() const { return mirror_; } QAbstractButton* ReceiverPanel::nrBox() const { return nr_; }
QAbstractButton* ReceiverPanel::anfBox() const { return anf_; } QAbstractButton* ReceiverPanel::nbBox() const { return nb_; }

void ReceiverPanel::setDialControlsRadio(bool on) { freq_->setLabelOnly(!on); freq_->setToolTip(on ? "TUNES THE RADIO: scroll over a digit, click a band, or click the spectrum" : "A label until a radio session is started: scroll over a digit to change it"); }
void ReceiverPanel::setPreampControlsRadio(bool on) { preamp_->setEnabled(on); if (!on) preamp_->setChecked(false); }
bool ReceiverPanel::preampOn() const { return preamp_->isChecked(); }
void ReceiverPanel::setPreampChecked(bool on) { preamp_->setChecked(on); }
void ReceiverPanel::lockIfOffset(bool locked) { if (locked) if_->setCurrentIndex(0); if_->setEnabled(!locked); }      // 0 = 9 kHz: the session tunes the DDS 9 kHz below the dial
void ReceiverPanel::setMuted(bool m) { mute_->setChecked(m); }
double ReceiverPanel::dialHz() const { return freq_->hz(); }
void ReceiverPanel::setDialHz(double hz) { freq_->setHz(hz); }                  // valueChanged -> axis, band highlight, dialChanged

RxOptions ReceiverPanel::options() const {
    RxOptions o; o.threaded = threaded_;
    o.rc.if_offset_hz = tx_on_dial_ ? tx_dds_below_ : if_->currentIndex() == 0 ? dsp::kIfOffsetHz : 0.0; o.rc.mirror = mirror_->isChecked();   // keyed: the DDS is on the dial
    if (tune_sw_) o.rc.if_offset_hz += (o.rc.mirror ? -1.0 : 1.0) * (dialHz() - ref_hz_);                                                      // a recording or the demo: tuning moves the receiver within the recording (a radio would move its DDS)
    o.dc.mode = static_cast<dsp::Mode>(modeIndex()); o.dc.agc = static_cast<dsp::AgcMode>(agcIndex()); o.dc.manual_gain_db = double(gain_->value()); o.dc.noise_blanker = nb_->isChecked();
    o.dc.noise_reduction = nr_->isChecked(); o.dc.spectral_nr = nr2_->isChecked() && decoderIndex() < 2; o.dc.auto_notch = anf_->isChecked(); o.dc.notch_hz = notch_list_.empty() ? 0.0 : notch_list_[0]; for (size_t i = 1; i < notch_list_.size(); ++i) o.dc.extra_notches_hz.push_back(notch_list_[i]);
    o.dc.squelch_dbfs = squelch_->value() <= -120 ? -200.0 : double(squelch_->value());
    const auto pb = dsp::passband_for(o.dc.mode, filter_width_, filter_low_); o.dc.lo_hz = pb.first; o.dc.hi_hz = pb.second;
    { static const double frac[] = {0.2, 0.1, 0.04}, floor_hz[] = {250, 150, 80}; o.dc.edge_fraction = frac[filter_shape_]; o.dc.edge_floor_hz = floor_hz[filter_shape_]; }
    { const int a = am_det_ ? am_det_->currentIndex() : 0; o.dc.am_detector = a == 3 ? dsp::AmDetector::Envelope : dsp::AmDetector::Sync;
      o.dc.am_sideband = a == 1 ? dsp::AmSideband::Lower : a == 2 ? dsp::AmSideband::Upper : dsp::AmSideband::Both; if (am_det_) am_det_->setVisible(modeIndex() == 2); }
    if (fm_dev_) { o.dc.fm_deviation_hz = fm_dev_->currentIndex() == 0 ? 2500.0 : 5000.0; o.dc.ctcss_hz = toneSquelchHz(); }
    if (fm_box_) fm_box_->setVisible(modeIndex() == 4);
    if (decoderIndex() == 1) o.dc.cw_pitch_hz = double(dec_tone_->value());              // the CW demodulator and the Morse decoder must agree on the pitch
    return o;
}
void ReceiverPanel::applySettings() {
    try {
        const bool was = playing_; const RxOptions o = options(); engine_.configure(o);
        spectrum_->setPassband(o.dc.lo_hz, o.dc.hi_hz); waterfall_->setPassband(o.dc.lo_hz, o.dc.hi_hz); updateSpectrumNotches();
        note_.clear(); if (was) setPlaying(true);
        noteBand();
    } catch (const std::exception& e) { note_ = QString("Cannot process this stream: ") + e.what() + " (needs a sample rate that is a multiple of 48000)"; setPlaying(false); }
}
void ReceiverPanel::settingsChanged() { applySettings(); updateStatus(); }

void ReceiverPanel::adopt(std::unique_ptr<IqSource> s, bool nineKhz, bool live) {
    rx_lost_sent_ = false;
    direct_rate_ = 0; syncRateButtons(0);                                // a new source is not the direct stream until loadLiveDirect says so
    forgetLiveDevice();                                                  // a new source: audio goes back to the PC until a live radio capture is open again
    setPlaying(false); const double fs = s->sample_rate(); engine_.set_source(std::move(s));
    scale_tracker_.reset(); scale_ = scale_tracker_.scale(); audio_total_ = 0; peak_hz_ = 0; peak_db_ = -200; signal_dbfs_ = -200;
    if (if_->isEnabled()) { QSignalBlocker b(if_); if_->setCurrentIndex(nineKhz ? 0 : 1); }   // while a radio session holds the IF offset locked (9 kHz), a new source cannot change it
    setSourceIsLive(live);
    if (rec_) rec_->sourceChanged(live, live ? fs : 0.0, 48000.0);                                    // live sources only: the replay buffer follows the stream
    spectrum_->setSpan(fs); waterfall_->setSpan(fs); setView(0, 1); ref_hz_ = dialHz(); tune_sw_ = !engine_.status().live; applySettings();       // the dial at load time is the recording's centre
    rebuildDecoder();                                                    // a fresh decoder for every new source: FT8 / FT4 anchor their UTC slots on the first sample,
}                                                                        // and a live stream reopened after a transmission has a gap the old one would not know about
QMap<QString, QString> ReceiverPanel::readSidecar(const QString& wav) {
    QMap<QString, QString> m; QFile f(QFileInfo(wav).path() + "/" + QFileInfo(wav).completeBaseName() + ".txt"); if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return m;
    const QStringList tok = QString::fromUtf8(f.read(8192)).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    for (const QString& t : tok) { const int eq = t.indexOf('='); if (eq > 0) m.insert(t.left(eq), t.mid(eq + 1)); }
    return m;
}
bool ReceiverPanel::loadFile(const QString& path) {                                 // any sample rate: odd ones are resampled automatically
    try {
        adopt(open_iq_file(path.toStdString(), 0, 1, loop_->isChecked()), false);
        const auto notes = readSidecar(path);                                                                     // a recording made by the app (or firesidecapture) says how to replay it
        if (!notes.isEmpty() && notes.value("kind") != "audio") {
            QStringList did; const bool hasMirror = notes.contains("mirror") || notes.contains("mirror_needed");
            if (hasMirror) { const bool m = notes.contains("mirror") ? notes.value("mirror") == "1" : notes.value("mirror_needed") == "yes"; { QSignalBlocker b(mirror_); mirror_->setChecked(m); } did << (m ? "mirror on" : "mirror off"); }
            if (notes.contains("if_offset_hz") && if_->isEnabled()) { const double off = notes.value("if_offset_hz").toDouble(); { QSignalBlocker b(if_); if_->setCurrentIndex(off >= 1000 ? 0 : 1); } did << QString("IF offset %1 kHz").arg(off / 1000.0, 0, 'g', 3); }
            double dial = notes.value("dial_hz").toDouble(); if (dial <= 0) dial = notes.value("dial_mhz").toDouble() * 1e6;
            if (dial > 0) { setDialHz(dial); ref_hz_ = dialHz(); did << QString("dial %1 MHz").arg(dial / 1e6, 0, 'f', 4); }                  // the recording's centre is that dial
            applySettings(); if (!did.isEmpty()) showNote("From the recording's notes: " + did.join(", "));
        }
        setPlaying(true); updateStatus(); return playing_;
    }
    catch (const std::exception& e) { note_ = QString("Cannot open: ") + e.what(); updateStatus(); return false; }
}
void ReceiverPanel::loadDemo() { adopt(std::make_unique<DemoIqSource>(48000), true, false); setPlaying(true); updateStatus(); }
bool ReceiverPanel::loadLive(const QString& device, double rate) {
    try {
        forgetLiveDevice();
        adopt(std::make_unique<PcmIqSource>(open_alsa_pcm(device.toStdString(), rate, 8, 1024), 0, 1, 1024), true, true);   // the radio (app or firesidecli CLI) tunes its DDS 9 kHz below the dial   // capture stream only
        live_device_ = device; live_rate_ = rate; setRadioOutputsAvailable(true);
        setPlaying(true); updateStatus(); return playing_;
    } catch (const std::exception& e) { note_ = QString("Cannot start capture: ") + e.what(); updateStatus(); return false; }
}
bool ReceiverPanel::loadLiveDirect(unsigned rate) {
    if (!wait_retry_) wait_rate_ = 0;                                            // chosen by hand: no longer waiting for the stream
    try {
        forgetLiveDevice();                                                          // the radio-jacks output needs snd-dice, which would take the stream back: PC audio only here
        std::unique_ptr<IPcm> pcm; FwIsoOptions o; o.duplex = !qEnvironmentVariableIsSet("FIRESIDE_NO_DUPLEX"); o.out_ring_seconds = 0.04;   // duplex: transmit on our own stream, receive never pauses
        auto open = [this](unsigned r, const FwIsoOptions& op) { return direct_opener_ ? direct_opener_(r, op) : open_fw_iso_pcm(r, op); };
        // A rate change on a running stream: our own stream holds the radio, so it is closed first, and the radio gets up to a second to let go
        // (2026-11-28: opening the new rate over the old one failed with "the radio is already streaming").
        const bool switching = direct_rate_ != 0 && engine_.status().live;
        if (switching) closeLive();
        if (switching && !direct_opener_) {
            for (int i = 0; i < 10; ++i) { try { pcm = open(rate, o); break; } catch (const std::exception&) { if (i == 9) throw; QThread::msleep(100); } }
        }
        if (pcm) {}
        else try { pcm = open(rate, o); } catch (const std::exception& e) { if (!o.duplex) throw; o.duplex = false; pcm = open(rate, o); note_ = QString("Duplex stream refused (%1): receive only; transmit will pause receive.").arg(e.what()); }
        adopt(std::make_unique<PcmIqSource>(std::move(pcm), 0, 1, 1024), true, true);
        live_rate_ = rate; direct_rate_ = rate; syncRateButtons(rate);
        if (fw_iso_duplex_running()) {                                                       // the radio's jacks are reachable now: offer them, and restore the last choice
            setRadioOutputsAvailable(true);
            const bool want = qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS") ? false : QSettings("fireside", "receiver").value("radio_audio", false).toBool();
            if (want) setAudioOutput(1, QSettings("fireside", "receiver").value("radio_audio_pair", 3).toInt(), true);
        }
        if (!qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) QSettings("fireside", "receiver").setValue("direct_rate", rate);
        setPlaying(true);
        if (note_.startsWith("Duplex stream refused")) { /* keep the reason */ }
        else { note_ = QString("Direct stream %1 kHz%2").arg(rate / 1000).arg(fw_iso_duplex_running() ? ", duplex" : "");   // short: the status line is one line
               appLog("receiver", QString("Direct FireWire stream at %1 kHz%2. Audio plays on the PC; the radio-jacks output needs the ALSA capture.").arg(rate / 1000).arg(fw_iso_duplex_running() ? ", duplex: transmit keeps receive running" : "")); }
        { const std::string n = fw_iso_last_open_note(); if (!n.empty()) { note_ += QString(" Note: %1.").arg(QString::fromStdString(n)); appLog("receiver", QString::fromStdString(n)); } }
        QTimer::singleShot(1500, this, [this, rate] {                                       // diagnostic (2026-11-28: after a rate switch the waterfall looked no wider)
            const auto st = engine_.status();
            appLog("receiver", QString("after the switch to %1 kHz: engine %2 Hz, %3 frames, fft %4; display span %5 Hz, view %6..%7, zoom %8")
                   .arg(rate / 1000).arg(st.sample_rate, 0, 'f', 0).arg(st.frames).arg(st.fft_size).arg(spectrum_->spanHz(), 0, 'f', 0)
                   .arg(spectrum_->viewLo(), 0, 'f', 3).arg(spectrum_->viewHi(), 0, 'f', 3).arg(zoomFactor(), 0, 'f', 2)); });
        updateStatus(); return playing_;
    } catch (const std::exception& e) { note_ = QString("Cannot start the direct stream: ") + e.what(); if (!wait_retry_) appLog("receiver", note_); updateStatus(); return false; }
}
void ReceiverPanel::syncRateButtons(unsigned rate) {
    static const unsigned k[3] = {48000, 96000, 192000};
    for (int i = 0; i < 3; ++i) { QSignalBlocker b(rate_btn_[i]); auto* g = rate_btn_[i]->group(); if (g) g->setExclusive(false); rate_btn_[i]->setChecked(rate == k[i]); if (g) g->setExclusive(true); }
}
QString ReceiverPanel::modeName() const { return kModeNames[std::clamp(modeIndex(), 0, 4)]; }
bool ReceiverPanel::mirrorOn() const { return mirror_->isChecked(); }
double ReceiverPanel::ifOffsetHz() const { return if_->currentIndex() == 0 ? 9000.0 : 0.0; }
QString ReceiverPanel::sourceText() const { return QString::fromStdString(engine_.status().source); }
void ReceiverPanel::showNote(const QString& text) { note_ = text; updateStatus(); QTimer::singleShot(12000, this, [this, text] { if (note_ == text) { note_.clear(); updateStatus(); } }); }
void ReceiverPanel::updateRecordButtons() {
    if (!rec_ || !grab_btn_) return; const bool av = rec_->available(), rc = rec_->recording();
    grab_btn_->setText(QString("Grab %1 s").arg(rec_->grabSeconds())); grab_btn_->setEnabled(av);
    rec_btn_->setEnabled(av || rc); rec_btn_->setText(rc ? "Stop" : "Rec"); rec_btn_->setProperty("rec", rc); rec_btn_->style()->unpolish(rec_btn_); rec_btn_->style()->polish(rec_btn_);
}
void ReceiverPanel::setRxAntennaText(const QString& name) { rx_ant_text_ = name; updateStatus(); }
void ReceiverPanel::setRadioRunning(bool on) { if (stop_radio_) stop_radio_->setEnabled(on); }
bool ReceiverPanel::openLiveAuto() {
    const FwIsoProbe direct = probe_fw_iso();
    if (direct.ok && !direct.busy) {
        const unsigned want = QSettings("fireside", "receiver").value("direct_rate", 192000).toUInt();
        const unsigned rate = std::find(direct.rates.begin(), direct.rates.end(), want) != direct.rates.end() ? want : direct.rates.back();
        if (loadLiveDirect(rate)) return true;
    }
    for (const auto& c : list_capture_cards()) if (c.flex_like) return loadLive(QString::fromStdString(c.device()), 48000);          // fallback: the radio's ALSA card
    if (note_.isEmpty()) { note_ = "No live source found for the radio."; updateStatus(); }
    return false;
}
void ReceiverPanel::openFile() {
    QSettings q("fireside", "receiver"); QString dir = q.value("last_dir").toString();                                   // the folder of the last recording, else ~/fireside-sdr/recordings
    if (dir.isEmpty() || !QDir(dir).exists()) dir = QDir(QDir::homePath() + "/fireside/recordings").exists() ? QDir::homePath() + "/fireside/recordings" : QDir::homePath();
    const QString p = QFileDialog::getOpenFileName(this, "Open I/Q recording", dir, "WAV files (*.wav);;All files (*)");
    if (!p.isEmpty()) { if (!qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) q.setValue("last_dir", QFileInfo(p).absolutePath()); loadFile(p); }
}
void ReceiverPanel::openLive() {
    const auto cards = list_capture_cards();
    const FwIsoProbe direct = probe_fw_iso();
    if (cards.empty() && !direct.ok) { note_ = QString("No capture source: no ALSA cards, and the direct FireWire stream is not available (%1).").arg(QString::fromStdString(direct.error)); updateStatus(); return; }
    QStringList names;
    if (direct.ok) names << QString("* FLEX-5000 direct FireWire stream, up to %1 kHz (bypasses snd-dice)%2").arg(direct.rates.back() / 1000).arg(direct.busy ? "  [busy: close the ALSA capture first]" : "");
    for (const auto& c : cards) names << QString("%1%2  [%3]").arg(c.flex_like ? "* " : "").arg(QString::fromStdString(c.name)).arg(QString::fromStdString(c.device()));
    bool ok = false; const QString pick = QInputDialog::getItem(this, "Live capture",
        "Capture source (* = the FLEX). Capture only: nothing is played or transmitted.\nThe direct stream sets the radio's stream clock (sound-chip registers, restored when it closes); no radio commands.", names, 0, false, &ok);
    if (!ok) return;
    const int idx = int(names.indexOf(pick));
    if (direct.ok && idx == 0) {
        QStringList rates; for (unsigned r : direct.rates) rates << QString::number(r);
        const QString last = QSettings("fireside", "receiver").value("direct_rate", rates.back()).toString();
        const QString rate = QInputDialog::getItem(this, "Live capture", "Sample rate (Hz) = width of band shown:", rates, std::max(0, int(rates.indexOf(last))), false, &ok);
        if (ok) loadLiveDirect(rate.toUInt());
        return;
    }
    const std::string dev = cards[size_t(idx - (direct.ok ? 1 : 0))].device();
    QStringList rates; { const auto caps = probe_alsa_pcm(dev); if (caps.ok) for (unsigned r : caps.rates_ok) rates << QString::number(r); }
    if (rates.isEmpty()) rates << "48000";                                          // probe failed or nothing listed: 48 kHz is the one rate known to work
    QString rate = rates.first();
    if (rates.size() > 1) { rate = QInputDialog::getItem(this, "Live capture", "Sample rate (Hz), only rates this card offers with 8 channels:", rates, 0, false, &ok); if (!ok) return; }
    loadLive(QString::fromStdString(dev), rate.toDouble());
}

void ReceiverPanel::setPlaying(bool on) {
    if (on) engine_.start(); else engine_.pause();
    playing_ = on && engine_.status().running;
    { QSignalBlocker b(play_); play_->setChecked(playing_); } play_->setText(playing_ ? "Pause" : "Play");
}
void ReceiverPanel::advance(size_t n) { engine_.advance(n); refresh(); }

void ReceiverPanel::refresh() {
    auto frames = engine_.take_frames();
    if (!frames.empty()) rx_bins_ = frames.back().size();
    if (tx_tap_) { tx_buf_.clear(); double rate = 0; tx_tap_->read(tx_buf_, &rate); if (tx_view_ && rate > 0 && !tx_buf_.empty()) drawTx(rate); }   // drained always: no stale over on the next key-down
    if (tx_view_) frames.clear();                    // keyed: the receiver hears only the radio's own leakage (no sensible level); the transmit picture is drawn instead
    const auto est = engine_.status();
    const float frame_dt = (est.fft_size && spectrum_->spanHz() > 0) ? float(double(est.fft_size / 2) / spectrum_->spanHz()) : 2048.f / 48000.f;   // seconds between spectrum frames (50 % overlap)
    // The waterfall scrolls at the same speed at every sample rate: one row per (fft/2)/48 kHz seconds, as at 48 kHz. At 96 / 192 kHz the
    // frames come 2 / 4 times as fast, so they are averaged (in dB) into one row (2026-11-25: it scrolled 4x faster at 192 kHz).
    const double row_dt = est.fft_size ? double(est.fft_size / 2) / 48000.0 : 2048.0 / 48000.0;
    for (const auto& db : frames) {
        scale_ = scale_tracker_.update(db, frame_dt);        // steady: grows within a second, shrinks over about 20 s, ignores silent frames, never wider than 140 dB
        if (row_acc_.size() != db.size()) { row_acc_.assign(db.size(), 0.0f); row_n_ = 0; row_t_ = 0; }
        for (size_t i = 0; i < db.size(); ++i) row_acc_[i] += db[i];
        ++row_n_; row_t_ += frame_dt;
        if (row_t_ + 1e-6 >= row_dt) {
            if (row_n_ == 1) waterfall_->pushRow(db, scale_);
            else { std::vector<float> row(row_acc_.size()); for (size_t i = 0; i < row.size(); ++i) row[i] = row_acc_[i] / float(row_n_); waterfall_->pushRow(row, scale_); }
            std::fill(row_acc_.begin(), row_acc_.end(), 0.0f); row_n_ = 0; row_t_ -= row_dt; if (row_t_ > row_dt) row_t_ = 0;
        }
    }
    if (!frames.empty()) { spectrum_->setScale(scale_); spectrum_->setFrame(frames.back()); }
    audio_buf_.clear(); engine_.take_audio(audio_buf_);
    if (data_out_ && !audio_buf_.empty()) data_out_->push(audio_buf_, data_gain_);                     // other programs: fixed level, not muted
    // The speaker gain: receive audio peaks around -12 dBFS, so the slider gets 10 dB of make-up (100 % = +10 dB; 32 % = unity). Before this,
    // headphones on a PC output were almost silent (2026-11-26: -35 dBFS peaks measured at the output). Clipping is clamped in the output.
    const double spk_gain = volume_->value() / 100.0 * 3.1623;
    {   // anti-VOX: how loud the speaker is now
        double pk = 0; if (!mute_->isChecked() && !tx_mute_) for (float v : audio_buf_) pk = std::max(pk, double(std::fabs(v)));
        if (!audio_buf_.empty()) speaker_db_ = 20 * std::log10(pk * spk_gain + 1e-10);
    }
    if (!mute_->isChecked() && !tx_mute_ && !audio_buf_.empty()) {
        if (radio_out_) { radio_out_->set_volume(float(std::min(1.0, spk_gain))); try { radio_out_->push(audio_buf_.data(), audio_buf_.size()); } catch (const std::exception& e) { note_ = QString("Radio audio stopped: ") + e.what(); radio_out_.reset(); QSignalBlocker b(out_target_); out_target_->setCurrentIndex(0); out_pair_->setEnabled(false); } }
        else audioOut_->push(audio_buf_, float(spk_gain));
    }
    const RxStatus st = engine_.status();
    peak_hz_ = st.peak_hz; peak_db_ = st.peak_db; signal_dbfs_ = st.signal_dbfs; chk_dbfs_ = st.signal_dbfs; chk_peak_hz_ = st.peak_hz; audio_total_ = size_t(st.audio_samples);
    if (playing_ && !st.running) { playing_ = false; QSignalBlocker b(play_); play_->setChecked(false); play_->setText("Play"); }
    if (st.ended) note_ = "End of recording."; else if (!st.error.empty()) note_ = QString("Stopped: ") + QString::fromStdString(st.error);
    if (!st.error.empty() && st.live && !rx_lost_sent_) { rx_lost_sent_ = true; appLog("receiver", "receive lost: " + QString::fromStdString(st.error)); emit receiveLost(QString::fromStdString(st.error)); }   // once per source: auto transmit stops (it cannot hear the answers)
    if (!st.error.empty() && st.live && direct_rate_ > 0 && !restart_pending_) restartDirect(QString::fromStdString(st.error));   // the direct stream died: bring it back
    wspr_dial_->store(dialHz() / 1e6);                                          // the WSPR decoder reads it when a slot is decoded (the frequencies it reports)
    { std::string t; if (engine_.take_text(t) > 0) addDecodedText(QString::fromStdString(t));
      if (decoderIndex() != 0) dec_status_->setText(st.decoder.empty() ? "Waiting for a signal..." : QString::fromStdString(st.decoder)); }
    meter_->setLevel(st.signal_dbfs <= -199 ? -120 : st.signal_dbfs);
    const double off = meterOffsetDb(); meter_->setOffsetDb(off); const bool cal = meterCalibrated();
    meter_text_->setText(st.signal_dbfs <= -199 ? "no signal" : QString("%1   %2 dBm %5   %3 dBFS   peak line %4 dBFS").arg(SMeter::sText(st.signal_dbfs + off), 0).arg(st.signal_dbfs + off, 0, 'f', 0).arg(st.signal_dbfs, 0, 'f', 1).arg(st.peak_db, 0, 'f', 1)
                         .arg(meterSource() == 2 ? "(checked)" : cal ? "(factory table)" : "(uncal)"));
    updateStatus();
}

size_t ReceiverPanel::waterfallRows() const { return waterfall_->rows(); }
QString ReceiverPanel::status() const { return status_->text(); }
void ReceiverPanel::updateStatus() {
    syncOutDest();
    if (note_ != logged_note_) { logged_note_ = note_; appLog("receiver", note_); }   // every new note, also to app.log
    const RxStatus st = engine_.status(); QString s;
    mode_chip_->setText(QString("%1  ·  %2  ·  AGC %3").arg(kModeNames[modeIndex()], width_text(filter_width_), QString(kAgcNames[agcIndex()]).toLower()));
    source_label_->setText(QString::fromStdString(st.source));
    if (st.fft_size && !st.source.empty()) s = QString("peak %1 dBFS at %2 Hz = %3   |   AGC gain %4 dB").arg(peak_db_, 7, 'f', 1).arg(peak_hz_, 7, 'f', 0)
                                                    .arg(QString::fromStdString(display::format_mhz(dialHz() + peak_hz_)), 13).arg(st.agc_gain_db, 6, 'f', 1);
    if (modeIndex() == 2 && am_det_ && am_det_->currentIndex() != 3 && st.fft_size && !st.source.empty())
        s += st.am_locked ? QString("   |   Sync AM locked, carrier %1 Hz").arg(st.am_carrier_hz, 6, 'f', 0) : QString("   |   Sync AM: no carrier lock");
    if (modeIndex() == 4 && st.fft_size && !st.source.empty()) {
        auto tone = [](double hz) { const char l = dsp::ctcss_uk_letter(hz); return QString::number(hz, 'f', 1) + " Hz" + (l ? QString(" (%1)").arg(l) : QString()); };
        s += "   |   FM: " + (st.ctcss_heard_hz > 0 ? "tone " + tone(st.ctcss_heard_hz) : QString("no tone"));
        if (toneSquelchHz() > 0 && !st.tone_open) s += ", tone squelch shut (wants " + tone(toneSquelchHz()) + ")";
        if (fm_rev_ && fm_rev_->isChecked()) s += ", REVERSE: listening on the input";
    }
    if (rec_ && rec_->recording()) s += (s.isEmpty() ? "" : "   |   ") + rec_->recordingText();
    if (!rx_ant_text_.isEmpty()) s += (s.isEmpty() ? "" : "   |   ") + QString("RX %1").arg(rx_ant_text_ == "RX1IN" ? QString("RX1 IN") : rx_ant_text_);
    if (!heard_note_.isEmpty()) s += (s.isEmpty() ? "" : "   |   ") + heard_note_;
    if (st.live) s += QString("   |   %1 xruns%2").arg(qulonglong(st.xruns), 5).arg(st.stream_dead ? ", STREAM DEAD (all zero > 1 s)" : "");
    if (st.frames_dropped || st.audio_dropped) s += QString("   |   dropped %1 frames, %2 audio").arg(qulonglong(st.frames_dropped), 5).arg(qulonglong(st.audio_dropped), 5);
    if (!note_.isEmpty()) s += (s.isEmpty() ? "" : "   |   ") + note_;
    if (status_->text() != s) { status_->setText(s); status_->setToolTip(s); }
}

// ---------------------------------------------------------------- keyboard helpers, memories, remembered settings
void ReceiverPanel::setView(double lo, double hi) { spectrum_->setView(lo, hi); waterfall_->setView(lo, hi); }
void ReceiverPanel::zoomView(int notches, double anchor) {
    const double lo = spectrum_->viewLo(), hi = spectrum_->viewHi(), w = hi - lo, span = std::max(1.0, spectrum_->spanHz());
    const double minw = std::min(1.0, std::max(1.0 / 128.0, 1500.0 / span));                  // never narrower than 1.5 kHz (or 1/128 of the span)
    const double nw = std::clamp(w * std::pow(0.8, notches), minw, 1.0);
    double nlo = anchor - (anchor - lo) * nw / w;                                               // the frequency under the mouse stays put
    nlo = std::clamp(nlo, 0.0, 1.0 - nw); if (nw >= 1.0) nlo = 0;
    setView(nlo, nlo + nw);
}
double ReceiverPanel::zoomFactor() const { return 1.0 / (spectrum_->viewHi() - spectrum_->viewLo()); }
void ReceiverPanel::setAxisSettling(int i) { i = std::clamp(i, 0, 2); { QSignalBlocker b(settle_); settle_->setCurrentIndex(i); } scale_tracker_.set_shrink_tau(i == 0 ? 5.f : i == 1 ? 20.f : 60.f); }
int ReceiverPanel::axisSettling() const { return settle_->currentIndex(); }
void ReceiverPanel::setPalette(int i) { palette_->setCurrentIndex(std::clamp(i, 0, display::kPaletteCount - 1)); }
int ReceiverPanel::paletteIndex() const { return palette_->currentIndex(); }
void ReceiverPanel::setFloorOffsetDb(int db) { floor_->setValue(db); }
void ReceiverPanel::stepDial(int steps) { setDialHz(dialHz() + double(steps) * step_hz_); }
void ReceiverPanel::nextBand(int dir) {
    const double hz = dialHz(); int cur = -1;
    for (int i = 0; i < int(std::size(kBands)); ++i) if (hz >= kBands[i].lo && hz <= kBands[i].hi) cur = i;
    int target = -1;
    if (cur >= 0) target = std::clamp(cur + (dir > 0 ? 1 : -1), 0, int(std::size(kBands)) - 1);
    else if (dir > 0) { for (int i = 0; i < int(std::size(kBands)); ++i) if (kBands[i].lo > hz) { target = i; break; } }       // between bands: the next one up / down
    else { for (int i = int(std::size(kBands)) - 1; i >= 0; --i) if (kBands[i].hi < hz) { target = i; break; } }
    if (target >= 0) recallBand(target);
}
int ReceiverPanel::bandIndexOf(double hz) const { int idx = -1; for (int i = 0; i < int(std::size(kBands)); ++i) if (hz >= kBands[i].lo && hz <= kBands[i].hi) idx = i; return idx; }
void ReceiverPanel::noteBand() {                                                          // where the dial is now, for its band (not while restoring or recalling)
    if (band_recall_ || restoring_ || recalling_) return;
    const int i = bandIndexOf(dialHz()); if (i < 0 || size_t(i) >= band_mem_.size()) return;
    band_mem_[size_t(i)] = {dialHz(), modeIndex(), filter_width_, filter_low_, decoderIndex(), true};
}
bool ReceiverPanel::recallBand(int i) {
    if (i < 0 || i >= int(std::size(kBands))) return false;
    const Band& b = kBands[i]; const BandMem m = bandMemory(i);
    if (!m.set || m.hz < b.lo || m.hz > b.hi) {                                           // never been here: its usual start, or the FT8 / FT4 frequency under that decoder
        double f = b.start_hz;
        if (isFtIndex(decoderIndex())) for (const auto& mk : fireside::bandplan::marks_between(b.lo, b.hi)) if (QString(mk.label) == (decoderIndex() == 5 ? "FT4" : "FT8")) { f = mk.hz; break; }
        if (decoderIndex() == 6 && fireside::wspr::dial_hz_for(b.start_hz) > 0) f = fireside::wspr::dial_hz_for(b.start_hz);
        setDialHz(f); return true;
    }
    band_recall_ = true; recalling_ = true;                                               // the old band keeps what it had; no auto sideband over the memory
    if (decoderIndex() != m.decoder) setDecoderIndex(m.decoder);                          // first: a decoder brings its own mode and filter
    setModeIndex(std::clamp(m.mode, 0, 4)); filter_low_ = m.low; setFilterWidth(m.width); setDialHz(m.hz);
    recalling_ = false; band_recall_ = false; noteBand(); syncFtButtons(); updateStatus(); return true;
}
int ReceiverPanel::saveMemory(const QString& name) {
    for (size_t i = 0; i < memories_.size(); ++i) if (memories_[i].name == name) { memories_[i] = currentMemory(name); refreshMemoryCombo(); mem_combo_->setCurrentIndex(int(i)); return int(i); }
    memories_.push_back(currentMemory(name)); refreshMemoryCombo(); mem_combo_->setCurrentIndex(int(memories_.size()) - 1); return int(memories_.size()) - 1;
}
bool ReceiverPanel::recallMemory(int i) {
    if (i < 0 || i >= int(memories_.size())) return false;
    const Memory m = memories_[size_t(i)]; leaveReverse(); recalling_ = true; if (m.mode == 4) applyFmMemory(m); setModeIndex(m.mode); filter_low_ = m.low; setFilterWidth(m.width); setDialHz(m.hz); recalling_ = false; noteBand(); return true;   // the memory's mode wins over Auto LSB / USB       // mode first: it resets the filter to the mode's default
}
void ReceiverPanel::deleteMemory(int i) { if (i >= 0 && i < int(memories_.size())) { memories_.erase(memories_.begin() + i); refreshMemoryCombo(); } }
void ReceiverPanel::refreshMemoryCombo() {
    QSignalBlocker b(mem_combo_); mem_combo_->clear();
    for (const auto& m : memories_) {
        QString extra; if (m.mode == 4) { if (m.shift_hz != 0) extra += QString(" %1%2 kHz").arg(m.shift_hz > 0 ? "+" : "").arg(m.shift_hz / 1e3, 0, 'f', 0); if (m.ttone_hz > 0) extra += QString(" T%1").arg(m.ttone_hz, 0, 'f', 1); if (m.tsq_hz > 0) extra += QString(" TSQ%1").arg(m.tsq_hz, 0, 'f', 1); }
        mem_combo_->addItem(QString("%1   (%2 MHz %3%4)").arg(m.name).arg(m.hz / 1e6, 0, 'f', 4).arg(kModeNames[m.mode]).arg(extra));
    }
}
static bool settings_disabled() { return qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS"); }
// ---------------------------------------------------------------- FM and repeaters (receive side)
const std::vector<ReceiverPanel::RepeaterChannel>& ReceiverPanel::ukRepeaterChannels() {
    static const std::vector<RepeaterChannel> ch = [] {
        std::vector<RepeaterChannel> v;
        v.push_back({"10 m FM calling 29.600 (simplex)", 29.600e6, 0});
        for (int i = 0; i < 8; ++i) { const double out = 29.620e6 + 10e3 * i; v.push_back({QString("10 m RH%1  out %2  in %3").arg(i + 1).arg(out / 1e6, 0, 'f', 3).arg((out - 100e3) / 1e6, 0, 'f', 3), out, -100e3}); }
        v.push_back({"6 m FM calling 51.510 (simplex)", 51.510e6, 0});
        for (int i = 0; i < 19; ++i) { const double out = 50.710e6 + 10e3 * i; v.push_back({QString("6 m  out %1  in %2").arg(out / 1e6, 0, 'f', 3).arg((out + 500e3) / 1e6, 0, 'f', 3), out, 500e3}); }
        return v;
    }();
    return ch;
}
void ReceiverPanel::buildFmControls(QVBoxLayout* col) {
    fm_box_ = new QWidget; auto* g = new QGridLayout(fm_box_); g->setContentsMargins(0, 4, 0, 0); g->setHorizontalSpacing(6); g->setVerticalSpacing(6);
    fm_dev_ = new QComboBox; fm_dev_->addItems({"Narrow ±2.5 kHz", "Wide ±5 kHz"}); fm_dev_->setCurrentIndex(1);
    fm_dev_->setToolTip("FM deviation the receiver expects: narrow for 12.5 kHz channels (most UK 10 m and 6 m repeaters), wide for 25 kHz. It sets the audio level and the filter (12 / 15 kHz).");
    auto tones = [](QComboBox* c, const QString& off) { c->addItem(off, 0.0); for (double hz : dsp::ctcss_tones()) { const char l = dsp::ctcss_uk_letter(hz); c->addItem(QString::number(hz, 'f', 1) + " Hz" + (l ? QString("  (UK %1)").arg(l) : QString()), hz); } };
    fm_tsq_ = new QComboBox; tones(fm_tsq_, "Tone squelch off"); fm_tsq_->setToolTip("Tone squelch (CTCSS decode): you only hear a signal carrying this tone, e.g. the repeater, not noise or other users. The status line shows the tone being heard. Works with the level squelch: both must open.");
    fm_ttone_ = new QComboBox; tones(fm_ttone_, "No transmit tone"); fm_ttone_->setToolTip("The CTCSS tone sent with every FM over (opens a repeater, or a station using tone squelch). Stored with the memories.");
    fm_shift_ = new QSpinBox; fm_shift_->setRange(-1000, 1000); fm_shift_->setSingleStep(100); fm_shift_->setSuffix(" kHz"); fm_shift_->setPrefix("Shift "); fm_shift_->setSpecialValueText("Simplex");
    fm_shift_->setToolTip("Repeater shift: its input minus its output (UK: 10 m -100 kHz, 6 m +500 kHz). Used by Reverse. Transmitting through a repeater (with the shift) is not in Fireside yet: FM transmit refuses while a shift is set, except in Reverse (on the input).");
    fm_rpt_ = new QComboBox; fm_rpt_->addItem("Repeater channel…"); for (const auto& c : ukRepeaterChannels()) fm_rpt_->addItem(c.name);
    fm_rpt_->setToolTip("UK band plan FM channels: tunes to the repeater output in FM and sets the shift. Set the repeater's tone yourself (it depends on the repeater).");
    fm_rev_ = new QPushButton("Reverse"); fm_rev_->setCheckable(true); fm_rev_->setToolTip("Listen on the repeater input (dial + shift): hear whether you could work the other station direct. Click again to go back.");
    g->addWidget(fm_dev_, 0, 0); g->addWidget(fm_tsq_, 0, 1); g->addWidget(fm_rpt_, 1, 0, 1, 2); g->addWidget(fm_shift_, 2, 0); g->addWidget(fm_ttone_, 2, 1); g->addWidget(fm_rev_, 3, 0);
    fm_box_->setVisible(false); col->addWidget(fm_box_);
    connect(fm_dev_, &QComboBox::activated, this, [this](int i) { if (modeIndex() == 4) setFilterWidth(i == 0 ? 12000 : 15000); else settingsChanged(); });
    connect(fm_tsq_, &QComboBox::activated, this, [this](int) { settingsChanged(); });
    connect(fm_rpt_, &QComboBox::activated, this, [this](int i) { if (i > 0) selectRepeater(i - 1); QSignalBlocker b(fm_rpt_); fm_rpt_->setCurrentIndex(0); });
    connect(fm_shift_, &QSpinBox::valueChanged, this, [this](int) { leaveReverse(); });
    connect(fm_rev_, &QPushButton::toggled, this, [this](bool on) {
        if (on) { const double sh = repeaterShiftHz(); if (sh == 0) { QSignalBlocker b(fm_rev_); fm_rev_->setChecked(false); note_ = "Reverse needs a repeater shift"; updateStatus(); return; }
                  rev_shift_hz_ = sh; setDialHz(dialHz() + sh); }
        else { setDialHz(dialHz() - rev_shift_hz_); rev_shift_hz_ = 0; }
        updateStatus();
    });
}
void ReceiverPanel::leaveReverse() { if (fm_rev_ && fm_rev_->isChecked()) fm_rev_->setChecked(false); }   // back to the output
double ReceiverPanel::toneSquelchHz() const { return fm_tsq_ ? fm_tsq_->currentData().toDouble() : 0.0; }
double ReceiverPanel::txToneHz() const { return fm_ttone_ ? fm_ttone_->currentData().toDouble() : 0.0; }
static void pick_tone(QComboBox* c, double hz) { int best = 0; for (int i = 1; i < c->count(); ++i) if (std::fabs(c->itemData(i).toDouble() - hz) < 0.05) best = i; c->setCurrentIndex(best); }
void ReceiverPanel::setToneSquelchHz(double hz) { pick_tone(fm_tsq_, hz); if (!restoring_) settingsChanged(); }
void ReceiverPanel::setTxToneHz(double hz) { pick_tone(fm_ttone_, hz); }
double ReceiverPanel::repeaterShiftHz() const { return fm_shift_ ? fm_shift_->value() * 1e3 : 0.0; }
void ReceiverPanel::setRepeaterShiftHz(double hz) { fm_shift_->setValue(int(std::lround(hz / 1e3))); }
double ReceiverPanel::ctcssHeardHz() const { return engine_.status().ctcss_heard_hz; }
bool ReceiverPanel::selectRepeater(int i) {
    const auto& ch = ukRepeaterChannels(); if (i < 0 || size_t(i) >= ch.size()) return false;
    leaveReverse(); setModeIndex(4); setRepeaterShiftHz(ch[size_t(i)].shift_hz); setDialHz(ch[size_t(i)].out_hz); return true;
}
ReceiverPanel::Memory ReceiverPanel::currentMemory(const QString& name) const {
    Memory m{name, dialHz(), modeIndex(), filter_width_, filter_low_};
    if (fm_rev_ && fm_rev_->isChecked()) m.hz -= rev_shift_hz_;                     // a memory is the repeater output, even when saved while reversed
    m.fm_dev = fm_dev_ ? fm_dev_->currentIndex() : 1; m.tsq_hz = toneSquelchHz(); m.ttone_hz = txToneHz(); m.shift_hz = repeaterShiftHz(); return m;
}
void ReceiverPanel::applyFmMemory(const Memory& m) {
    QSignalBlocker b1(fm_dev_), b2(fm_tsq_); fm_dev_->setCurrentIndex(std::clamp(m.fm_dev, 0, 1)); pick_tone(fm_tsq_, m.tsq_hz); setTxToneHz(m.ttone_hz); setRepeaterShiftHz(m.shift_hz);
}

void ReceiverPanel::saveSettings() {
    if (settings_disabled()) return;
    QSettings q("fireside", "receiver");
    q.setValue("dial_hz", dialHz()); q.setValue("mode", modeIndex()); q.setValue("agc", agcIndex()); q.setValue("manual_gain_db", gain_->value()); q.setValue("filter", filter_width_); q.setValue("filter_low", filter_low_); q.setValue("shape", filter_shape_); q.setValue("step", steps_->checkedId());
    q.setValue("nb", nb_->isChecked()); q.setValue("nr", nr_->isChecked()); q.setValue("nr2", nr2_->isChecked()); q.setValue("anf", anf_->isChecked()); q.setValue("mirror_live", mirror_live_); q.setValue("mirror_file", mirror_file_); if (loop_) q.setValue("loop", loop_->isChecked());
    q.setValue("notch", notch_->value()); { QStringList nl; for (double f : notch_list_) nl << QString::number(f); q.setValue("notches", nl.join(",")); } q.setValue("squelch", squelch_->value()); q.setValue("decoder", decoderIndex()); q.setValue("decoder_tone", dec_tone_->value()); q.setValue("my_call", my_call_->text()); q.setValue("am_detector", am_det_->currentIndex()); q.setValue("palette", palette_->currentIndex()); q.setValue("axis_settle", settle_->currentIndex()); q.setValue("tx_open", transmitOpen()); q.setValue("floor", floor_->value()); q.setValue("bandplan", bandplan_->isChecked()); q.setValue("spectrum_avg", avg_->currentIndex()); q.setValue("volume", volume_->value()); q.setValue("muted", mute_->isChecked()); if (auto_sb_) q.setValue("auto_sideband", auto_sb_->isChecked());
    q.setValue("fm_deviation", fm_dev_->currentIndex()); q.setValue("fm_tone_squelch_hz", toneSquelchHz()); q.setValue("fm_tx_tone_hz", txToneHz()); q.setValue("fm_shift_hz", repeaterShiftHz());
    if (if_ && if_->isEnabled()) q.setValue("if_index", if_->currentIndex());                // not while a radio session holds it locked (9 kHz)
    q.beginWriteArray("memories"); for (size_t i = 0; i < memories_.size(); ++i) { q.setArrayIndex(int(i)); q.setValue("name", memories_[i].name); q.setValue("hz", memories_[i].hz); q.setValue("mode", memories_[i].mode); q.setValue("width", memories_[i].width); q.setValue("low", memories_[i].low); q.setValue("fm_dev", memories_[i].fm_dev); q.setValue("tsq_hz", memories_[i].tsq_hz); q.setValue("ttone_hz", memories_[i].ttone_hz); q.setValue("shift_hz", memories_[i].shift_hz); } q.endArray();
    q.beginWriteArray("band_memory"); int k = 0;
    for (size_t i = 0; i < band_mem_.size() && i < std::size(kBands); ++i) { const BandMem& m = band_mem_[i]; if (!m.set) continue;
        q.setArrayIndex(k++); q.setValue("band", kBands[i].name); q.setValue("hz", m.hz); q.setValue("mode", m.mode); q.setValue("width", m.width); q.setValue("low", m.low); q.setValue("decoder", m.decoder); }
    q.endArray();
}
void ReceiverPanel::loadSettings() {
    if (settings_disabled()) return;
    QSettings q("fireside", "receiver"); restoring_ = true;
    if (q.contains("mode")) setModeIndex(std::clamp(q.value("mode").toInt(), 0, 4));
    if (q.contains("filter")) filter_width_ = q.value("filter").toDouble();
    if (q.contains("filter_low")) filter_low_ = q.value("filter_low").toDouble();
    am_det_->setCurrentIndex(std::clamp(q.value("am_detector", 0).toInt(), 0, 3));
    { QSignalBlocker b1(fm_dev_), b2(fm_tsq_), b3(fm_ttone_), b4(fm_shift_); fm_dev_->setCurrentIndex(std::clamp(q.value("fm_deviation", 1).toInt(), 0, 1));
      setToneSquelchHz(q.value("fm_tone_squelch_hz", 0.0).toDouble()); setTxToneHz(q.value("fm_tx_tone_hz", 0.0).toDouble()); setRepeaterShiftHz(q.value("fm_shift_hz", 0.0).toDouble()); }
    if (q.contains("shape")) { filter_shape_ = std::clamp(q.value("shape").toInt(), 0, 2); QSignalBlocker b(shape_box_); shape_box_->setCurrentIndex(filter_shape_); }
    rebuildFilterButtons();
    gain_->setValue(std::clamp(q.value("manual_gain_db", 60).toInt(), 0, 100)); setAgcIndex(std::clamp(q.value("agc", 2).toInt(), 0, 3));
    if (auto* b = steps_->button(std::clamp(q.value("step", 1).toInt(), 0, 3))) b->setChecked(true);
    nb_->setChecked(q.value("nb", false).toBool()); nr_->setChecked(q.value("nr", false).toBool()); nr2_->setChecked(q.value("nr2", false).toBool()); anf_->setChecked(q.value("anf", false).toBool()); mirror_live_ = q.value("mirror_live", true).toBool(); mirror_file_ = q.value("mirror_file", q.value("mirror", false).toBool()).toBool(); mirror_->setChecked(source_live_ ? mirror_live_ : mirror_file_);
    loop_->setChecked(q.value("loop", true).toBool()); { std::vector<double> nl; for (const QString& t : q.value("notches", q.value("notch", 0).toString()).toString().split(",", Qt::SkipEmptyParts)) { const double f = t.toDouble(); if (f > 0) nl.push_back(f); } notch_list_ = nl; syncNotchBox(); } squelch_->setValue(q.value("squelch", -120).toInt());
    setAxisSettling(q.value("axis_settle", 1).toInt()); setTransmitOpen(q.value("tx_open", false).toBool());
    palette_->setCurrentIndex(std::clamp(q.value("palette", 0).toInt(), 0, display::kPaletteCount - 1)); floor_->setValue(std::clamp(q.value("floor", 0).toInt(), -30, 30)); bandplan_->setChecked(q.value("bandplan", true).toBool()); avg_->setCurrentIndex(std::clamp(q.value("spectrum_avg", 1).toInt(), 0, 3)); volume_->setValue(q.value("volume", 60).toInt()); mute_->setChecked(q.value("muted", false).toBool());
    if (if_->isEnabled()) if_->setCurrentIndex(std::clamp(q.value("if_index", 0).toInt(), 0, 1));
    if (q.contains("dial_hz")) setDialHz(q.value("dial_hz").toDouble());
    my_call_->setText(q.value("my_call").toString().trimmed().toUpper()); if (auto_sb_) auto_sb_->setChecked(q.value("auto_sideband", true).toBool());                                      // before the decoder below is rebuilt with it
    if (q.contains("decoder") && q.value("decoder").toInt() > 0) { setDecoderIndex(q.value("decoder").toInt()); dec_tone_->setValue(q.value("decoder_tone", dec_tone_->value()).toInt()); rebuildDecoder(); }
    memories_.clear(); const int n = q.beginReadArray("memories");
    for (int i = 0; i < n; ++i) { q.setArrayIndex(i); memories_.push_back({q.value("name").toString(), q.value("hz").toDouble(), std::clamp(q.value("mode").toInt(), 0, 4), q.value("width").toDouble(), q.value("low", 150.0).toDouble(),
                                                              std::clamp(q.value("fm_dev", 1).toInt(), 0, 1), q.value("tsq_hz", 0.0).toDouble(), q.value("ttone_hz", 0.0).toDouble(), q.value("shift_hz", 0.0).toDouble()}); }
    q.endArray(); refreshMemoryCombo();
    { const int nb = q.beginReadArray("band_memory");
      for (int i = 0; i < nb; ++i) { q.setArrayIndex(i); const QString name = q.value("band").toString();
          for (size_t j = 0; j < std::size(kBands) && j < band_mem_.size(); ++j) if (name == kBands[j].name) {
              const double hz = q.value("hz").toDouble(); if (hz < kBands[j].lo || hz > kBands[j].hi) break;
              band_mem_[j] = {hz, std::clamp(q.value("mode").toInt(), 0, 4), q.value("width", 2400.0).toDouble(), q.value("low", 150.0).toDouble(), std::clamp(q.value("decoder", 0).toInt(), 0, 5), true}; } }
      q.endArray(); }
    restoring_ = false; applySettings();
}

// ---------------------------------------------------------------- decoders
int ReceiverPanel::decoderIndex() const { return dec_mode_->currentIndex(); }
QString ReceiverPanel::decodedText() const { return dec_text_->toPlainText(); }
void ReceiverPanel::clearDecoded() { dec_text_->clear(); clearCallers(); }
void ReceiverPanel::setExternalMode(bool on) {
    external_ = on; { QSignalBlocker b(ext_box_); ext_box_->setChecked(on); }
    dec_mode_->setEnabled(!on);
    if (on && decoderIndex() != 0) setDecoderIndex(0);
    dec_status_->setText(on ? "External program: it decodes, logs and spots" : (decoderIndex() == 0 ? "Decoder off" : dec_status_->text()));
}
bool ReceiverPanel::setDataOutput(bool on, double level_db) {
    data_gain_ = float(std::pow(10.0, std::clamp(level_db, -40.0, 0.0) / 20.0));
    if (!on) { if (data_out_) { data_out_->deleteLater(); data_out_ = nullptr; } return true; }
    if (data_out_) return true;
    auto* o = new AudioOut(true, this, "Fireside RX audio");
    if (!o->available()) { o->deleteLater(); return false; }
    data_out_ = o; return true;
}
void ReceiverPanel::setDecoderTone(int hz) { dec_tone_->setValue(hz); rebuildDecoder(); applySettings(); }
void ReceiverPanel::setDecoderIndex(int i) {
    QTimer::singleShot(0, this, [this] { syncFtButtons(); });
    const int was = decoderIndex();
    i = std::clamp(i, 0, 6);
    if (external_ && i > 0) {                                                   // External program: the mode and filter a decoder brings, but no decoder of ours
        if (i == 1 && modeIndex() != 3) setModeIndex(3);
        if (i >= 2 && modeIndex() != 0) setModeIndex(0);
        if (i >= 4) filter_width_ = 3600, rebuildFilterButtons();
        i = 0;
    }
    { QSignalBlocker b(dec_mode_); dec_mode_->setCurrentIndex(i); }
    if (i != was) { clearDecoded(); clearQso(); }                                   // another mode: the decodes and the QSO pane start empty (user 2026-11-27)
    ext_box_->setVisible(i != 6);                                                   // WSPR: no other program is needed (we decode, upload and beacon ourselves)
    static const int tone[] = {700, 700, 1000, 2125, 1500, 1500, 1500};
    if (i > 0) dec_tone_->setValue(tone[i]);
    if (i == 1 && modeIndex() != 3) setModeIndex(3);                                                       // Morse: CW mode (tone at the pitch)
    if (i >= 2 && modeIndex() != 0) setModeIndex(0);                                                          // PSK31 / RTTY / FT8 / FT4: always USB (LSB too: digital modes are USB on every band), tones inside the voice filter
    if (i >= 4) filter_width_ = 3600, rebuildFilterButtons();                                                        // FT8 spans 200-3000 Hz (WSPR sits at 1400-1600): the widest voice filter (applySettings below)
    dec_text_->setVisible(i != 0); if (callers_) callers_->setVisible(isFtIndex(i)); clearCallers(); if (qso_text_) qso_text_->setVisible(isFtIndex(i)); if (clear_qso_) clear_qso_->setVisible(isFtIndex(i));
    if (tx_side_) { dec_text_->setMinimumHeight(i >= 4 ? 110 : 60); dec_text_->setMaximumHeight(QWIDGETSIZE_MAX); }   // beside the transmit card: fill its height
    else dec_text_->setFixedHeight(i >= 4 ? 190 : 84); dec_tone_->setVisible(i != 0 && i < 4);   // CW / PSK31 / RTTY only: FT8 and FT4 decode the whole passband    // FT8 prints a dozen lines per slot dec_status_->setText(i == 0 ? "Decoder off" : "Waiting for a signal...");
    rebuildDecoder(); applySettings(); syncGroup(); emit decoderSelected(i);
}
QString ReceiverPanel::myCall() const {                                    // the Decoder card's box, else the Log tab's station callsign (from Wavelog)
    const QString c = my_call_->text().trimmed().toUpper(); if (!c.isEmpty() || qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) return c;
    return QSettings("fireside", "log").value("my_call").toString().trimmed().toUpper();
}
void ReceiverPanel::rebuildDecoder() {
    switch (decoderIndex()) {
        case 1: { decode::CwOptions o; o.pitch_hz = dec_tone_->value(); engine_.set_decoder(std::make_unique<decode::CwDecoder>(o)); break; }
        case 2: { decode::PskOptions o; o.center_hz = dec_tone_->value(); engine_.set_decoder(std::make_unique<decode::Psk31Decoder>(o)); break; }
        case 3: { decode::RttyOptions o; o.mark_hz = dec_tone_->value(); engine_.set_decoder(std::make_unique<decode::RttyDecoder>(o)); break; }
        case 4: { if (!engine_.status().live) engine_.with_source([](IqSource* s) { if (s) s->rewind(); });   // a recording is taken to start on a slot boundary, so start it from the top
                  decode::Ft8Options o; o.wall_clock = engine_.status().live; o.my_call = myCall().toStdString(); { QPointer<ReceiverPanel> me(this); o.late_lines = [me](const std::string& t) { const QString q = QString::fromStdString(t); QMetaObject::invokeMethod(qApp, [me, q] { if (me) me->addDecodedText(q); }, Qt::QueuedConnection); }; } /* a cut-short slot's weak decodes, later */ engine_.set_decoder(std::make_unique<decode::Ft8Decoder>(o)); break; }   // live audio follows UTC slots; a file is taken to start at a slot boundary
        case 5: { if (!engine_.status().live) engine_.with_source([](IqSource* s) { if (s) s->rewind(); });
                  decode::Ft8Options o; o.ft4 = true; o.wall_clock = engine_.status().live; engine_.set_decoder(std::make_unique<decode::Ft8Decoder>(o)); break; }
        case 6: { decode::WsprOptions o; o.wall_clock = engine_.status().live; o.my_call = myCall().toStdString(); wspr_dial_->store(dialHz() / 1e6); o.dial_now = wspr_dial_; o.dial_mhz = dialHz() / 1e6;
                  if (!o.wall_clock) engine_.with_source([](IqSource* s) { if (s) s->rewind(); });   // a recording is taken to start on an even minute
                  QPointer<ReceiverPanel> me(this); const bool live = o.wall_clock;
                  o.on_slot = [me, live](const decode::WsprSlot& w) {                                // the decode thread: hand the slot to the window's thread
                      QStringList lines; for (const auto& sp : w.spots) lines << QString::fromStdString(fireside::wspr::format(sp));
                      const QString d = QString::fromStdString(w.date), h = QString::fromStdString(w.hhmm), err = QString::fromStdString(w.error); const double mhz = w.dial_mhz; const bool ok = w.ok;
                      if (live) QMetaObject::invokeMethod(qApp, [me, d, h, mhz, lines, ok, err] { if (me) emit me->wsprSlot(d, h, mhz, lines, ok, err); }, Qt::QueuedConnection); };
                  engine_.set_decoder(std::make_unique<decode::WsprDecoder>(o)); break; }
        default: engine_.set_decoder(nullptr);
    }
    pushFocus();                                                                          // a new decoder: tell it where the stations calling us are
}
void ReceiverPanel::noteFocus(const QString& call, const QString& line) {     // a station calling us: its audio frequency (the decoder line's 4th field)
    const QStringList f = line.split(' ', Qt::SkipEmptyParts); if (f.size() < 4 || call.isEmpty()) return;
    bool ok = false; const double hz = f[3].toDouble(&ok); if (!ok || hz < 100 || hz > 4000) return;
    focus_[call] = {hz, QDateTime::currentMSecsSinceEpoch()}; pushFocus();
}
std::vector<double> ReceiverPanel::decoderFocus() const {
    std::vector<double> v; const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!qso_partner_.isEmpty() && focus_.contains(qso_partner_)) v.push_back(focus_[qso_partner_].first);   // the partner first
    for (auto it = focus_.begin(); it != focus_.end(); ++it) if (it.key() != qso_partner_ && now - it.value().second < 180000 && v.size() < 6) v.push_back(it.value().first);
    return v;
}
void ReceiverPanel::pushFocus() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = focus_.begin(); it != focus_.end();) { if (it.key() != qso_partner_ && now - it.value().second >= 180000) it = focus_.erase(it); else ++it; }   // 3 minutes, the partner kept
    engine_.set_decoder_focus(decoderFocus());
}

// ---------------------------------------------------------------- audio output: PC sound or the radio's jacks
void ReceiverPanel::setSourceIsLive(bool live) {
    source_live_ = live; QSignalBlocker b(mirror_); mirror_->setChecked(live ? mirror_live_ : mirror_file_);      // the caller applies the settings
}
void ReceiverPanel::restartDirect(const QString& why) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!restarts_ || now - first_restart_ms_ > kRestartWindowMs) { restarts_ = 0; first_restart_ms_ = now; }
    if (++restarts_ > kMaxRestarts) {
        if (restarts_ == kMaxRestarts + 1) appLog("receiver", QString("direct stream stopped again (%1): %2 restarts in 2 min, not trying again. Press Stop radio, then start it.").arg(why).arg(kMaxRestarts));
        note_ = QString("Stopped: %1. Restarted %2 times in 2 min without luck: press Stop radio, then start it again.").arg(why).arg(kMaxRestarts); updateStatus(); return;
    }
    restart_pending_ = true; const unsigned rate = direct_rate_;
    appLog("receiver", QString("direct stream stopped (%1): restarting it at %2 kHz (try %3 of %4)").arg(why).arg(rate / 1000).arg(restarts_).arg(kMaxRestarts));
    note_ = QString("Stream stopped (%1): restarting it ...").arg(why); updateStatus();
    QTimer::singleShot(500, this, [this, rate] {                                 // a moment for the radio; Stop radio meanwhile leaves it closed
        restart_pending_ = false;
        if (direct_rate_ != rate) return;                                        // closed or changed by hand in the meantime
        closeLive();
        if (loadLiveDirect(rate)) { appLog("receiver", "direct stream restarted"); note_ = QString("Stream restarted by itself (try %1 of %2).").arg(restarts_).arg(kMaxRestarts); updateStatus(); }
        else if (note_.contains("already streaming")) waitForStream(rate, QDateTime::currentMSecsSinceEpoch() + kWaitMs);
    });
}
void ReceiverPanel::waitForStream(unsigned rate, qint64 until_ms) {
    if (wait_rate_ != rate) appLog("receiver", QString("another program has the radio's stream: looking again every %1 s for %2 min, and taking it back once it is free").arg(wait_poll_ms_ / 1000.0).arg(kWaitMs / 60000));
    wait_rate_ = rate;
    note_ = "Another program has the radio's stream (snd-dice / ALSA?): waiting for it to let go ..."; updateStatus();
    QTimer::singleShot(int(wait_poll_ms_), this, [this, rate, until_ms] {
        if (wait_rate_ != rate || engine_.status().live) return;                 // Stop radio, a source chosen by hand, or a pause for transmit: no longer ours to reopen
        wait_retry_ = true; const bool ok = loadLiveDirect(rate); wait_retry_ = false;
        if (ok) { wait_rate_ = 0; restarts_ = 0; appLog("receiver", "the radio's stream is free again: direct stream restarted"); note_ = "Stream taken back after another program let go of it."; updateStatus(); return; }
        if (!note_.contains("already streaming") || QDateTime::currentMSecsSinceEpoch() > until_ms) {
            wait_rate_ = 0; appLog("receiver", "stopped waiting for the radio's stream: " + note_);
            note_ = "Stopped: " + note_ + " Press Stop radio, then start it again."; updateStatus(); return;
        }
        waitForStream(rate, until_ms);
    });
}
bool ReceiverPanel::closeLive() {
    wait_rate_ = 0;                                                               // Stop radio / a pause for transmit: stop waiting for the stream
    if (!engine_.status().live) return false;
    { engine_.finish_decoder(); std::string t; if (engine_.take_text(t) > 0) addDecodedText(QString::fromStdString(t)); }   // the slot heard so far (e.g. an answer just before we transmit) is decoded, not lost
    setPlaying(false); forgetLiveDevice(); engine_.set_source(nullptr);
    { const std::string n = fw_iso_last_close_note(); appLog("receiver", QString("live source closed%1").arg(n.empty() ? QString(" (radio stream registers restored)") : QString(": ") + QString::fromStdString(n))); } setSourceIsLive(false); direct_rate_ = 0; syncRateButtons(0); if (rec_) rec_->sourceChanged(false, 0, 0);                // dropping the source closes the capture device
    note_ = "Live capture closed."; updateStatus(); return true;
}
void ReceiverPanel::forgetLiveDevice() {
    radio_out_.reset(); live_device_.clear();
    { QSignalBlocker a(out_target_), b(out_pair_); out_target_->setCurrentIndex(0); out_pair_->setCurrentIndex(0); }
    out_pair_->setEnabled(false); setRadioOutputsAvailable(false);
}
void ReceiverPanel::setRadioOutputsAvailable(bool on) {
    if (auto* m = qobject_cast<QStandardItemModel*>(out_target_->model())) m->item(1)->setEnabled(on);
    if (out_dest_) if (auto* m = qobject_cast<QStandardItemModel*>(out_dest_->model())) for (int i = 1; i < 4; ++i) m->item(i)->setEnabled(on);
}
void ReceiverPanel::syncOutDest() {
    if (!out_dest_) return;
    const int pair = out_pair_->currentIndex(); const int d = out_target_->currentIndex() != 1 ? 0 : pair == 3 ? 1 : pair == 2 ? 2 : pair == 4 ? 3 : 0;
    QSignalBlocker b(out_dest_); out_dest_->setCurrentIndex(d);
}
void ReceiverPanel::outputControlsChanged() {
    const bool ok = setAudioOutput(out_target_->currentIndex(), out_pair_->currentIndex(), false);
    if (!qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS") && fw_iso_duplex_running()) { QSettings q("fireside", "receiver"); q.setValue("radio_audio", out_target_->currentIndex() == 1 && ok); if (ok) q.setValue("radio_audio_pair", out_pair_->currentIndex()); }   // remembered for the next start
}
bool ReceiverPanel::setAudioOutput(int target, int pair, bool confirmed) {
    radio_out_.reset(); pair = std::clamp(pair, 0, 4);
    const bool duplex = fw_iso_duplex_running();                                            // the direct stream also sends to the radio: audio goes with it
    { QSignalBlocker a(out_target_), b(out_pair_); out_target_->setCurrentIndex(target == 1 && (duplex || !live_device_.isEmpty()) ? 1 : 0); out_pair_->setCurrentIndex(pair); }
    out_pair_->setEnabled(out_target_->currentIndex() == 1);
    if (out_target_->currentIndex() != 1) { if (target == 1) note_ = "Open a Live capture first: it names the radio's card."; updateStatus(); return target != 1; }
    if (AudioOut::radioPhones()) { note_ = "Everything already plays on the radio's headphone jack (Audio tab output): choose a PC output there to use the radio's jacks here.";
                                   QSignalBlocker a(out_target_); out_target_->setCurrentIndex(0); out_pair_->setEnabled(false); updateStatus(); return false; }
    if (duplex) {
        if (pair == 1) { note_ = "Channels 0/1 carry the transmit I/Q: choose 4/5 (external speaker), 2/3 (headphones) or 6/7."; QSignalBlocker b(out_pair_); out_pair_->setCurrentIndex(0); updateStatus(); return false; }
        if (pair == 0) { note_ = "Radio output selected: choose the channel pair (4/5 = external speaker)."; updateStatus(); return false; }
        try {
            radio_out_ = std::make_unique<RadioAudioOut>(open_fw_iso_out(FwIsoPcm::OutKind::Audio), pair - 1, 48000.0);   // the audio ring: it can only reach channels 2..7
            note_ = QString("Audio now plays from the radio (%1).").arg(QString::fromStdString(radio_out_->describe())); updateStatus(); return true;
        } catch (const std::exception& e) { note_ = QString("Cannot play to the radio: ") + e.what() + " (audio stays on the PC)"; QSignalBlocker a(out_target_), b(out_pair_); out_target_->setCurrentIndex(0); out_pair_->setEnabled(false); updateStatus(); return false; }
    }
    if (pair == 0) { note_ = "Radio output selected: choose the channel pair (after the output-channel test). Audio stays on the PC until then."; updateStatus(); return false; }
    if (!confirmed) {
        const auto r = QMessageBox::question(this, "Play audio from the radio?",
            QString("Send the receive audio to the radio's output channels %1/%2?\n\nOnly do this after the one-channel-at-a-time output test on the real radio (tools/firesideplay): the manual's two channel tables disagree, "
                    "so a wrong pair may be silent or come out of the wrong jack. It cannot key the transmitter. Keep the headphone volume low the first time.").arg(2 * (pair - 1)).arg(2 * (pair - 1) + 1),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (r != QMessageBox::Yes) { QSignalBlocker a(out_target_), b(out_pair_); out_target_->setCurrentIndex(0); out_pair_->setCurrentIndex(0); out_pair_->setEnabled(false); return false; }
    }
    try {
        radio_out_ = std::make_unique<RadioAudioOut>(open_alsa_play(live_device_.toStdString(), live_rate_, 8, 1024), pair - 1);
        note_ = QString("Audio now plays from the radio (%1).").arg(QString::fromStdString(radio_out_->describe())); updateStatus(); return true;
    } catch (const std::exception& e) {
        note_ = QString("Cannot play to the radio: ") + e.what() + " (audio stays on the PC)"; { QSignalBlocker a(out_target_), b(out_pair_); out_target_->setCurrentIndex(0); out_pair_->setCurrentIndex(0); } out_pair_->setEnabled(false); updateStatus(); return false;
    }
}

namespace {
const QString kInfoSep = QStringLiteral("  \u2502 ");        // decoder line | tag | callbook info
constexpr int kMsgCols = 50, kTagCols = 15;                 // "HHMMSS  SNR   DT FREQ ~  " is 24 columns, the longest messages ~24 more; the longest tag "new band/mode" is 13
// "M0OML", "M0OML/P", "EA8/M0OML", "<M0OML>" are all me
bool is_me(const std::string& to, const QString& me) {
    if (me.isEmpty()) return false;
    QString t = QString::fromStdString(to).toUpper(); if (t.startsWith('<') && t.endsWith('>')) t = t.mid(1, t.size() - 2);
    for (const QString& part : t.split('/')) if (part == me) return true;
    return t == me;
}
}  // namespace
void ReceiverPanel::addDecodedText(const QString& t0) {
    QString t = t0;
    QString me = myCall(); if (const int sl = me.indexOf('/'); sl >= 0) { const QStringList p = me.split('/'); me = *std::max_element(p.begin(), p.end(), [](const QString& a, const QString& b) { return a.size() < b.size(); }); }   // the base call of M0OML/P
    if (decoderIndex() == 6) {                                                  // WSPR: each spot with its distance from my grid; a hashed call that wsprd knew, shown plain
        static const QRegularExpression hashed("<([A-Z0-9/]+)>");
        QStringList out, calls; const qint64 now = QDateTime::currentSecsSinceEpoch();
        for (QString l : t0.split('\n')) {
            if (l.trimmed().isEmpty()) { out << l; continue; }
            l.replace(hashed, "\\1");
            if (const auto sp = fireside::wspr::parse_wsprd_line(l.toStdString()); sp && sp->call.find('<') == std::string::npos) {
                const QString b = QString::fromStdString(fireside::qrz::base_call(sp->call));
                if (!b.isEmpty()) { calls << b; if (sp->grid.size() >= 4) heard_grids_.insert(b, {QString::fromStdString(sp->grid).toUpper(), now}); }
            }
            out << l + wsprDistance(l);
        }
        t = out.join('\n');
        dec_text_->moveCursor(QTextCursor::End); dec_text_->insertPlainText(t); dec_text_->moveCursor(QTextCursor::End);
        calls.removeDuplicates(); for (const QString& c : calls) emit wsprCall(c);   // after the slot's decode: the callbook, then its line is filled in
        return;
    }
    if (isFtIndex(decoderIndex())) {                                            // FT8 / FT4: fixed columns (message | tag | callbook), filled in later in place
        QStringList out;
        for (const QString& l : t0.split('\n')) {
            const std::string txt = fireside::logbook::ftx_line_text(l.toStdString());
            if (!txt.empty()) { const auto m = fireside::logbook::parse_ftx_message(txt); if (m.ok && is_me(m.from, me)) continue; }   // our own over, decoded off the radio's leakage: not shown (it is in the QSO pane already), not spotted (user 2026-11-25)
            out << (txt.empty() || l.contains(kInfoSep) ? l : l.leftJustified(kMsgCols) + kInfoSep + QString(kTagCols, ' '));
        }
        t = out.join('\n');
        if (t.trimmed().isEmpty()) return;
    }
    dec_text_->moveCursor(QTextCursor::End); dec_text_->insertPlainText(t); dec_text_->moveCursor(QTextCursor::End);
    if (!isFtIndex(decoderIndex())) return;
    const QStringList lines = t.split('\n', Qt::SkipEmptyParts);
    QTextCharFormat hf; hf.setBackground(QColor("#7a1f2b")); hf.setForeground(QColor("#ffffff")); hf.setFontWeight(QFont::Bold);
    QTextBlockFormat hb; hb.setBackground(QColor("#7a1f2b"));
    QTextBlock b = dec_text_->document()->lastBlock();                       // the new lines are the last ones (the trailing newline leaves an empty block)
    std::vector<QTextBlock> blocks; for (int n = 0; b.isValid() && int(blocks.size()) < lines.size() && n < lines.size() + 2; b = b.previous(), ++n) if (!b.text().trimmed().isEmpty()) blocks.push_back(b);
    std::reverse(blocks.begin(), blocks.end());
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const QTextBlock& blk : blocks) {                                   // first remember the grids they sent (the callbook text for these lines uses them)
        const auto m = fireside::logbook::parse_ftx_message(fireside::logbook::ftx_line_text(blk.text().toStdString()));
        if (m.ok && !m.grid.empty() && m.grid != "RR73") { const QString b = QString::fromStdString(fireside::qrz::base_call(m.from)); if (!b.isEmpty()) heard_grids_.insert(b, {QString::fromStdString(m.grid).toUpper(), now}); }
    }
    for (const QTextBlock& blk : blocks) {
        const QString ln = blk.text(); const auto m = fireside::logbook::parse_ftx_message(fireside::logbook::ftx_line_text(ln.toStdString()));
        if (!m.ok || m.from.empty()) continue;
        { const QStringList fw = ln.split(' ', Qt::SkipEmptyParts); bool okf = false; const double hz = fw.size() > 3 ? fw[3].toDouble(&okf) : 0;
          if (okf && hz > 0 && !is_me(m.from, me)) heard_ftx_.append({QDateTime::currentMSecsSinceEpoch(), hz, ln.left(6)}); }   // where stations are (a clear frequency for our CQ); our own over, decoded off the radio's leakage while keyed, is not one
        const bool mentions_me = !me.isEmpty() && (is_me(m.to, me) || is_me(m.from, me) || ln.left(ln.indexOf(kInfoSep) < 0 ? ln.size() : ln.indexOf(kInfoSep)).contains(" " + me + " "));
        if (m.cq || (mentions_me && !is_me(m.from, me))) emit decodedCall(QString::fromStdString(m.from));     // worked-before / callbook: CQ callers and stations mentioning me only (user 2026-11-20)
        if (m.cq) emit cqDecoded(QString::fromStdString(m.from));
        if (source_live_) emit ftxLine(ln, dialHz(), decoderIndex() == 5 ? QString("FT4") : QString("FT8"));
        const bool to_me = !m.cq && is_me(m.to, me) && !is_me(m.from, me);
        if (!to_me && !is_me(m.from, me) && qsoActiveNow() && (m.cq || (!m.to.empty() && !is_me(m.to, me)))) {   // during a QSO, a station that called us is heard calling someone else (or CQ): it has moved on
            const QString fb = QString::fromStdString(fireside::qrz::base_call(m.from));
            for (int qi = 0; qi < caller_list_.size(); ++qi) if (caller_list_[qi].call == fb && !caller_list_[qi].manual) { caller_list_.removeAt(qi); renderCallers(); break; }
        }
        if (to_me) {
            QTextCursor c(blk); c.mergeBlockFormat(hb); c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor); c.mergeCharFormat(hf);
            emit callerDecoded(ln);
        }
        const QString shown = (ln.indexOf(kInfoSep) < 0 ? ln : ln.left(ln.indexOf(kInfoSep))).trimmed();
        if (to_me) {
            const QString fb = QString::fromStdString(fireside::qrz::base_call(m.from));
            // the QSO pane: what is addressed to us and our own overs; during a QSO only the partner's (anyone else calling waits in the callers area)
            const QStringList mw = QString::fromStdString(fireside::logbook::ftx_line_text(ln.toStdString())).split(' ', Qt::SkipEmptyParts);
            const bool signoff = mw.size() >= 3 && (mw[2] == "73" || mw[2] == "RR73" || mw[2] == "RRR");
            if (signoff && fb != qso_partner_ && addToDoneQso(fb, shown)) {}          // a finished QSO's late 73: inside that QSO's block, above its "done" line
            else if (qso_partner_.isEmpty() || fb == qso_partner_) addQsoLine(shown, 1);
            noteFocus(fb, shown);                                                 // the quick decode before our next over looks harder at its frequency
            const QStringList words = QString::fromStdString(fireside::logbook::ftx_line_text(ln.toStdString())).split(' ', Qt::SkipEmptyParts);
            const bool closing = words.size() >= 3 && (words[2] == "73" || words[2] == "RR73" || words[2] == "RRR");   // a sign-off (e.g. a late 73 after our QSO ended) is not a call
            if (!fb.isEmpty() && fb != qso_partner_ && !closing) addCaller(fb, shown);   // someone new calling us: kept in the callers area until answered (the partner's replies are in the QSO pane)
        }
    }
}

qint64 ReceiverPanel::callersNow() const { return callers_now_ ? callers_now_() : QDateTime::currentMSecsSinceEpoch(); }
void ReceiverPanel::addCaller(const QString& call, const QString& line) {
    const qint64 now = callersNow();
    if (moved_on_.contains(call)) {                                                               // worked or removed by hand: not again for 10 min
        const qint64 ago = now - moved_on_.value(call);
        if (ago < kMovedOnMs) {                                                                    // said, so a red line that is not queued is never a mystery
            if (!kept_out_said_.contains(call)) { kept_out_said_.insert(call);
                const QString why = QString("%1 called you: not queued (worked or removed %2 min ago; back after 10 min, or double-click the line)").arg(call).arg(std::max<qint64>(1, ago / 60000));
                appLog("receiver", why); note_ = why; updateStatus(); }
            return;
        }
        moved_on_.remove(call); kept_out_said_.remove(call);
    }
    for (auto& c : caller_list_) if (c.call == call) { c.line = line; c.ms = now; c.manual = false; renderCallers(); return; }   // a repeat call: its newest line, same place
    caller_list_.append({call, line, info_by_call_.value(call), tag_by_call_.value(call, -1), now, false}); renderCallers();      // a new caller joins at the bottom
}
void ReceiverPanel::queueTop(const QString& call, const QString& line) {
    if (call.isEmpty() || call == qso_partner_) return;
    moved_on_.remove(call);                                                                       // the operator's own choice wins over the 10-minute rest
    for (int i = 0; i < caller_list_.size(); ++i) if (caller_list_[i].call == call) { caller_list_.removeAt(i); break; }
    caller_list_.prepend({call, line, info_by_call_.value(call), tag_by_call_.value(call, -1), callersNow(), true}); renderCallers();
}
void ReceiverPanel::bumpCaller(const QString& call) {
    for (int i = 1; i < caller_list_.size(); ++i) if (caller_list_[i].call == call) { caller_list_.swapItemsAt(i, i - 1); renderCallers(); return; }
}
void ReceiverPanel::dropCaller(const QString& call) { if (call.isEmpty()) return; removeCaller(call); moved_on_.insert(call, callersNow()); kept_out_said_.remove(call); }
void ReceiverPanel::removeCaller(const QString& call) {
    if (call.isEmpty()) return;
    for (int i = 0; i < caller_list_.size(); ++i) if (caller_list_[i].call == call) { caller_list_.removeAt(i); renderCallers(); return; }
}
void ReceiverPanel::clearCallers() { caller_list_.clear(); moved_on_.clear(); kept_out_said_.clear(); renderCallers(); }
void ReceiverPanel::tickCallers() { fadeQsos(); }                                  // the queue keeps its stations until worked, removed or moved on (no time limit)
QString ReceiverPanel::callersText() const {
    QStringList out;
    for (const auto& c : caller_list_) { QString l = c.line; if (c.tag >= 0 || !c.info.isEmpty()) l += kInfoSep + (c.tag >= 0 ? QString(fireside::award::tag_text(c.tag)) : QString()).leftJustified(kTagCols) + c.info; out << l; }
    return out.join('\n');
}
static QColor tagColour(int tag) {
    switch (tag) {
        case 0: return QColor("#3ddc84"); case 1: return QColor("#ffb454"); case 2: return QColor("#8b93a7");
        case fireside::award::kNewDxcc: return QColor("#ff4fd8"); case fireside::award::kNewDxccBand: return QColor("#ff9be6");
        case fireside::award::kNewCont: return QColor("#c792ea"); case fireside::award::kNewCq: case fireside::award::kNewItu: return QColor("#4fd6f7");
        default: return QColor("#82aaff");                                                    // new grid, new grid on this band
    }
}
void ReceiverPanel::renderCallers() {
    if (!queue_rows_) return;
    while (caller_list_.size() > kMaxCallers) caller_list_.removeLast();                      // a very busy pile-up: the latest arrivals wait outside
    while (queue_rows_->count() > 1) { QLayoutItem* it = queue_rows_->takeAt(0); if (it->widget()) it->widget()->deleteLater(); delete it; }   // (the stretch stays last)
    const int n = int(caller_list_.size());
    queue_title_->setText(n ? QString("QUEUE  (%1 waiting: the top is next)").arg(n) : QString("QUEUE  (empty: stations calling you wait here)"));
    for (int i = 0; i < n; ++i) {
        const Caller& cl = caller_list_[i];
        auto* row = new QFrame; row->setObjectName("queueRow"); row->setCursor(Qt::ArrowCursor);
        row->setStyleSheet(cl.manual ? "#queueRow { background:#2a3a5a; }" : "#queueRow { background:#7a1f2b; }");   // queued by hand: blue; called us: red
        auto* h = new QHBoxLayout(row); h->setContentsMargins(4, 1, 4, 1); h->setSpacing(4);
        auto* up = new QToolButton; up->setText(QString::fromUtf8("▲")); up->setToolTip("Move up one place"); up->setEnabled(i > 0); up->setAutoRaise(true);
        auto* x = new QToolButton; x->setText(QString::fromUtf8("✕")); x->setToolTip("Remove from the queue (not listed again for 10 minutes)"); x->setAutoRaise(true);
        auto* lab = new QLabel; lab->setFont(dec_text_->font()); lab->setTextFormat(Qt::RichText); lab->setCursor(Qt::ArrowCursor);
        QString html = QString("<span style='color:#ffffff; font-weight:bold;'>%1</span>").arg(cl.line.toHtmlEscaped().replace(' ', "&nbsp;"));
        if (cl.tag >= 0 || !cl.info.isEmpty()) {
            html += QString("<span style='color:#aab3c5;'>%1</span>").arg(kInfoSep.toHtmlEscaped().replace(' ', "&nbsp;"));
            if (cl.tag >= 0) html += QString("<span style='color:%1;%2'>%3</span>").arg(tagColour(cl.tag).name(), cl.tag >= fireside::award::kNewDxcc ? " font-weight:bold;" : "",
                                                                                      QString(fireside::award::tag_text(cl.tag)).leftJustified(kTagCols).toHtmlEscaped().replace(' ', "&nbsp;"));
            html += QString("<span style='color:#aab3c5;'>%1</span>").arg(cl.info.toHtmlEscaped());
        }
        if (cl.manual) html += "<span style='color:#9fb4d8;'>&nbsp;&nbsp;(queued by hand)</span>";
        lab->setText(html); lab->setProperty("queue_line", cl.line); lab->installEventFilter(this); row->setProperty("queue_line", cl.line); row->installEventFilter(this);
        h->addWidget(up); h->addWidget(x); h->addWidget(lab, 1);
        const QString call = cl.call;
        connect(up, &QToolButton::clicked, this, [this, call] { bumpCaller(call); });
        connect(x, &QToolButton::clicked, this, [this, call] { dropCaller(call); });
        queue_rows_->insertWidget(queue_rows_->count() - 1, row);
    }
}
void ReceiverPanel::activateLine(const QString& line0, bool from_queue) {
    QString line = line0; if (const int sep = line.indexOf(kInfoSep); sep >= 0) line = line.left(sep);
    if (line.trimmed().isEmpty()) return;
    if (!from_queue && qsoActiveNow()) {                                                          // during a QSO: queue them at the top, the QSO carries on
        const auto m = fireside::logbook::parse_ftx_message(fireside::logbook::ftx_line_text(line.toStdString()));
        const QString b = m.ok ? QString::fromStdString(fireside::qrz::base_call(m.from)) : QString();
        if (!b.isEmpty() && b != qso_partner_ && !is_me(m.from, myCall())) { queueTop(b, line.trimmed()); note_ = QString("%1 queued at the top: answered when this QSO ends").arg(b); updateStatus(); return; }
    }
    emit decodedLineActivated(line);
}
bool ReceiverPanel::qsoPaneAtBottom(int* maximum) const {
    if (!qso_text_) return true; auto* sb = qso_text_->verticalScrollBar(); if (maximum) *maximum = sb->maximum(); return sb->value() == sb->maximum();
}
void ReceiverPanel::addQsoLine(const QString& line, int kind) {       // kind 1 = to me (highlighted), 2 = our transmission (kind 0, plain, is no longer used)
    if (!qso_text_) return;
    qso_text_->moveCursor(QTextCursor::End); QTextCursor c = qso_text_->textCursor();
    if (!qso_text_->document()->isEmpty()) c.insertBlock();
    QTextCharFormat f; QTextBlockFormat bf;
    if (kind == 1) { f.setForeground(QColor("#ffffff")); f.setFontWeight(QFont::Bold); bf.setBackground(QColor("#7a1f2b")); }
    else if (kind == 2) { f.setForeground(QColor("#ffb454")); bf.setBackground(QColor("#2b2414")); }
    else f.setForeground(QColor("#c9d1dc"));
    c.setBlockFormat(bf); c.insertText(line, f); qso_text_->moveCursor(QTextCursor::End); qso_text_->verticalScrollBar()->setValue(qso_text_->verticalScrollBar()->maximum());
}
void ReceiverPanel::setTxView(bool keyed, double below, double k) {
    tx_k_ = k; tx_view_below_ = below;
    if (keyed == tx_view_) return;
    tx_view_ = keyed; tx_row_t_ = 0; tx_pending_.clear();
    spectrum_->setAveraging(spectrum_->averaging());                  // a fresh average: the transmit and receive pictures never mix
    spectrum_->setUnit(keyed && k > 0 ? "dBm TX" : keyed ? "dBFS TX" : "dBFS");
    if (!keyed) spectrum_->setScale(scale_);
}
void ReceiverPanel::drawTx(double rate) {
    namespace dsp = fireside::dsp;
    const size_t nrx = rx_bins_ ? rx_bins_ : 4096; const double span = spectrum_->spanHz() > 0 ? spectrum_->spanHz() : rate;
    size_t n = 256; while (n < 65536 && double(n) < double(nrx) * rate / span) n *= 2;   // about the receive picture's bin width
    if (!tx_an_ || tx_an_rate_ != rate || tx_an_->size() != n) {
        tx_an_ = std::make_unique<dsp::SpectrumAnalyzer>(rate, n); tx_an_rate_ = rate;
        tx_an_->on_frame = [this](const std::vector<float>& db) { if (tx_pending_.size() < 64) tx_pending_.push_back(db); };
    }
    tx_an_->push(tx_buf_.data(), tx_buf_.size());
    const float add = tx_k_ > 0 ? float(10.0 * std::log10(tx_k_) + 30.0) : 0.0f;   // dBFS of drive -> dBm at the antenna: P = k |x|^2
    const fireside::display::Scale sc = tx_k_ > 0 ? fireside::display::Scale{-20.f, 56.f} : fireside::display::Scale{-120.f, 0.f};   // fixed while keyed: 56 dBm = 400 W, so 100 W (50 dBm) sits near the top
    const double row_dt = double(nrx / 2) / 48000.0, hop = double(n / 2) / rate, bw = rate / double(n);
    for (const auto& db : tx_pending_) {
        std::vector<float> row(nrx, -200.f);
        for (size_t i = 0; i < nrx; ++i) {
            const double f = (double(i) - double(nrx) / 2.0) * span / double(nrx) + tx_view_below_;   // offset from the dial -> offset from the TX DDS
            const long k = std::lround(f / bw) + long(n / 2); if (k >= 0 && k < long(n)) row[i] = db[size_t(k)] + add;
        }
        tx_row_t_ += hop; if (tx_row_t_ + 1e-6 >= row_dt) { waterfall_->pushRow(row, sc); tx_row_t_ -= row_dt; if (tx_row_t_ > row_dt) tx_row_t_ = 0; }
        tx_last_ = std::move(row);
    }
    if (!tx_pending_.empty()) { spectrum_->setScale(sc); spectrum_->setFrame(tx_last_); }
    tx_pending_.clear();
}
void ReceiverPanel::setTransmitting(bool keyed, double below) {
    if (keyed == tx_on_dial_ && (!keyed || below == tx_dds_below_)) return;
    tx_on_dial_ = keyed; tx_dds_below_ = keyed ? below : 0; const QString keep = note_; applySettings(); note_ = keep; updateStatus();
}
void ReceiverPanel::peekDecode() { engine_.peek_decoder(); std::string t; if (engine_.take_text(t) > 0) addDecodedText(QString::fromStdString(t)); }
void ReceiverPanel::clearQso() { if (qso_text_) qso_text_->clear(); qso_partner_.clear(); done_qsos_.clear(); qso_start_block_ = -1; emit qsoCleared(); }
void ReceiverPanel::setQsoPartner(const QString& call) {
    const QString b = QString::fromStdString(fireside::qrz::base_call(call.toStdString()));
    if (b == qso_partner_) return;
    qso_partner_ = b; removeCaller(b); pushFocus();                            // replying to a caller answers them; the partner's frequency goes first
    if (!b.isEmpty() && qso_text_) { qso_text_->moveCursor(QTextCursor::End); QTextCursor c = qso_text_->textCursor(); if (!qso_text_->document()->isEmpty()) c.insertBlock();
        QTextCharFormat f; f.setForeground(QColor("#8b93a7")); c.setBlockFormat(QTextBlockFormat()); c.insertText(QString::fromUtf8("\u2500\u2500 QSO with %1 \u2500\u2500").arg(b), f); if (info_by_call_.contains(b)) c.insertText("  " + info_by_call_.value(b), f); qso_start_block_ = qso_text_->document()->blockCount() - 1; qso_text_->verticalScrollBar()->setValue(qso_text_->verticalScrollBar()->maximum()); }
}
void ReceiverPanel::endQso(const QString& call, bool done) {               // the sequence finished: a closing line, and the pane takes callers again
    const QString b = QString::fromStdString(fireside::qrz::base_call(call.toStdString()));
    if (!b.isEmpty()) { removeCaller(b); if (done) { moved_on_.insert(b, callersNow()); kept_out_said_.remove(b); } }   // off the callers list; worked: not back for 10 min. Given up on: back as soon as they call again (they may have heard us late)
    if (qso_text_) { qso_text_->moveCursor(QTextCursor::End); QTextCursor c = qso_text_->textCursor(); if (!qso_text_->document()->isEmpty()) c.insertBlock();
        QTextCharFormat f; f.setForeground(done ? QColor("#3ddc84") : QColor("#8b93a7")); f.setFontWeight(QFont::Bold); c.setBlockFormat(QTextBlockFormat());
        c.insertText(done ? QString::fromUtf8("\u2500\u2500 QSO with %1 done \u2500\u2500").arg(b) : QString::fromUtf8("\u2500\u2500 QSO with %1 not completed (no answer) \u2500\u2500").arg(b), f);
        qso_text_->verticalScrollBar()->setValue(qso_text_->verticalScrollBar()->maximum()); }
    if (qso_text_) {                                                                // the finished QSO fades out like a caller does, then goes
        DoneQso d; d.call = b; d.last = qso_text_->document()->blockCount() - 1; d.first = qso_start_block_ >= 0 && qso_start_block_ <= d.last ? qso_start_block_ : d.last; d.ms = callersNow();
        for (int i = d.first; i <= d.last; ++i) { const QTextBlock bl = qso_text_->document()->findBlockByNumber(i);
            d.bg.push_back(bl.blockFormat().background().style() == Qt::NoBrush ? QColor("#12161d") : bl.blockFormat().background().color());
            d.fg.push_back(bl.begin().atEnd() ? QColor("#c9d1dc") : bl.begin().fragment().charFormat().foreground().color()); }
        done_qsos_.append(d); qso_start_block_ = -1;
    }
    if (b == qso_partner_) qso_partner_.clear();
}
bool ReceiverPanel::addToDoneQso(const QString& call, const QString& line) {   // the newest finished QSO with this station, still on screen
    if (!qso_text_ || call.isEmpty()) return false;
    int k = int(done_qsos_.size()) - 1; while (k >= 0 && done_qsos_[k].call != call) --k;
    if (k < 0 || done_qsos_[k].last < 1 || done_qsos_[k].last <= done_qsos_[k].first) return false;
    DoneQso& d = done_qsos_[k]; const int at = d.last;                                  // the new line takes the "done" line's place; that moves down one
    QTextDocument* doc = qso_text_->document(); const QTextBlock before = doc->findBlockByNumber(at - 1); if (!before.isValid()) return false;
    QTextCursor c(before); c.movePosition(QTextCursor::EndOfBlock);
    QTextCharFormat f; f.setForeground(QColor("#ffffff")); f.setFontWeight(QFont::Bold); QTextBlockFormat bf; bf.setBackground(QColor("#7a1f2b"));
    c.insertBlock(bf, f); c.insertText(line, f);
    const size_t j = size_t(at - d.first); d.bg.insert(d.bg.begin() + long(j), QColor("#7a1f2b")); d.fg.insert(d.fg.begin() + long(j), QColor("#ffffff"));
    ++d.last;
    for (int o = 0; o < done_qsos_.size(); ++o) if (o != k) { if (done_qsos_[o].first >= at) ++done_qsos_[o].first; if (done_qsos_[o].last >= at) ++done_qsos_[o].last; }
    if (qso_start_block_ >= at) ++qso_start_block_;
    fadeQsos();                                                                         // at the block's current shade at once
    return true;
}
void ReceiverPanel::fadeQsos() {                                                    // same time as the callers: 6 FT8 / 10 FT4 cycles
    if (!qso_text_ || done_qsos_.isEmpty()) return;
    const bool ft4 = decoderIndex() == 5; const double span_ms = (ft4 ? 10 : 6) * (ft4 ? 7500.0 : 15000.0); const qint64 now = callersNow();
    auto mix = [](const QColor& a, const QColor& b, double t) { return QColor(int(a.red() + (b.red() - a.red()) * t), int(a.green() + (b.green() - a.green()) * t), int(a.blue() + (b.blue() - a.blue()) * t)); };
    QTextDocument* doc = qso_text_->document();
    for (int k = done_qsos_.size() - 1; k >= 0; --k) {                            // newest first; removing a range moves the later ranges up (fixed below)
        DoneQso& d = done_qsos_[k]; const double t = std::clamp(double(now - d.ms) / span_ms, 0.0, 1.0);
        if (t >= 1.0) {
            QTextCursor c(doc->findBlockByNumber(d.first)); const QTextBlock lastb = doc->findBlockByNumber(d.last);
            c.setPosition(lastb.position() + lastb.length() - 1, QTextCursor::KeepAnchor);
            if (lastb.next().isValid()) c.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);   // and the line break after it
            else if (d.first > 0) { c.setPosition(doc->findBlockByNumber(d.first - 1).position() + doc->findBlockByNumber(d.first - 1).length() - 1); c.setPosition(lastb.position() + lastb.length() - 1, QTextCursor::KeepAnchor); }
            c.removeSelectedText();
            const int n = d.last - d.first + 1; const int first = d.first; done_qsos_.removeAt(k);
            for (auto& o : done_qsos_) if (o.first > first) { o.first -= n; o.last -= n; }
            if (qso_start_block_ > first) qso_start_block_ -= n;
            continue;
        }
        for (int i = d.first; i <= d.last; ++i) {
            const QTextBlock bl = doc->findBlockByNumber(i); if (!bl.isValid()) continue; const size_t j = size_t(i - d.first);
            QTextCursor c(bl); QTextBlockFormat bf = bl.blockFormat(); bf.setBackground(mix(d.bg[j], QColor("#12161d"), t)); c.setBlockFormat(bf);
            c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor); QTextCharFormat cf; cf.setForeground(mix(d.fg[j], QColor("#4a5261"), t)); c.mergeCharFormat(cf);
        }
    }
}
QList<QPair<double, QString>> ReceiverPanel::recentFtx(qint64 window_ms) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    while (!heard_ftx_.isEmpty() && now - heard_ftx_.front().ms > 600000) heard_ftx_.removeFirst();   // at most 10 min kept
    QList<QPair<double, QString>> out; for (const auto& h : heard_ftx_) if (now - h.ms <= window_ms) out.append({h.hz, h.label});
    return out;
}
QPair<QString, QString> ReceiverPanel::nextCaller(const QSet<QString>& skip) const {   // the top of the queue not tried yet: {call, decoded line}
    for (int i = 0; i < caller_list_.size(); ++i) if (!skip.contains(caller_list_[i].call) && caller_list_[i].call != qso_partner_) return {caller_list_[i].call, caller_list_[i].line};
    return {};
}
void ReceiverPanel::addQsoTx(const QString& utc, int hz, const QString& msg) {
    { QString to = msg.section(' ', 0, 0).trimmed(); to.remove('<').remove('>'); removeCaller(QString::fromStdString(fireside::qrz::base_call(to.toStdString()))); }   // our over to them answers them
    addQsoLine(QString("%1  TX      %2 ~  %3").arg(utc).arg(hz, 4).arg(msg), 2);
}
QString ReceiverPanel::qsoPaneText() const { return qso_text_ ? qso_text_->toPlainText() : QString(); }
bool ReceiverPanel::selectFt(bool ft4) {
    const double hz = dialHz(); int idx = -1; for (int i = 0; i < int(std::size(kBands)); ++i) if (hz >= kBands[i].lo && hz <= kBands[i].hi) idx = i;
    if (idx < 0) for (int i = 0; i < int(std::size(kBands)); ++i) if (QString(kBands[i].name) == "20") idx = i;     // between bands: 20 m
    double f = 0; for (const auto& m : fireside::bandplan::marks_between(kBands[idx].lo, kBands[idx].hi)) if (QString(m.label) == (ft4 ? "FT4" : "FT8")) { f = m.hz; break; }
    if (f <= 0) { note_ = QString("No %1 frequency on %2 m").arg(ft4 ? "FT4" : "FT8", kBands[idx].name); updateStatus(); syncFtButtons(); return false; }
    setDecoderIndex(ft4 ? 5 : 4);                                                         // USB and the wide filter come with the decoder
    setDialHz(f); syncFtButtons(); return true;
}
bool ReceiverPanel::selectWspr() {
    double f = fireside::wspr::dial_hz_for(dialHz()); if (f <= 0) f = fireside::wspr::dial_hz_for(14.1e6);     // between bands: 20 m
    setDecoderIndex(6); setDialHz(f); syncFtButtons(); return true;
}
QString ReceiverPanel::wsprDistance(const QString& line) const {
    const auto sp = fireside::wspr::parse_wsprd_line(line.toStdString()); const QString mine = my_grid_src_ ? my_grid_src_() : QString();
    double la1, lo1, la2, lo2, km, deg;
    if (!sp || sp->grid.empty() || mine.size() < 4 || !fireside::qrz::grid_latlon(mine.toStdString(), la1, lo1) || !fireside::qrz::grid_latlon(sp->grid, la2, lo2)) return QString();
    fireside::qrz::distance_bearing(la1, lo1, la2, lo2, km, deg); return QString("  %1 km").arg(km, 0, 'f', 0);
}
void ReceiverPanel::syncFtButtons() {
    if (!ft8_btn_) return; QSignalBlocker a(ft8_btn_), b(ft4_btn_), c(psk_btn_), d(rtty_btn_);
    if (wspr_btn_) { QSignalBlocker w(wspr_btn_); wspr_btn_->setChecked(decoderIndex() == 6); }
    ft8_btn_->setChecked(decoderIndex() == 4); ft4_btn_->setChecked(decoderIndex() == 5); psk_btn_->setChecked(decoderIndex() == 2); rtty_btn_->setChecked(decoderIndex() == 3);
}
void ReceiverPanel::setHeardNote(const QString& t) { if (t != heard_note_) { heard_note_ = t; updateStatus(); } }
void ReceiverPanel::setAntennaWidget(QWidget* body) {
    if (!header_) return; auto* l = new QVBoxLayout; l->setSpacing(4); l->addWidget(body);
    header_->insertWidget(header_->indexOf(signal_card_), theme::makeCard("Antenna and tuner", l), 0);       // top row: after Frequency, Mode, Band; before the meters
}
void ReceiverPanel::setTransmitMeters(QWidget* power, QLabel* status, QWidget* arm) {   // one tile: the signal meter while receiving, the power meter while keyed (and 5 s after); the transmit status runs under both
    status->setFixedHeight(status->fontMetrics().height() + 2);
    tx_meter_ = power; meters_grid_->addWidget(power, 0, 0); power->setVisible(false);
    auto* sr = new QHBoxLayout; sr->setSpacing(8); sr->addWidget(arm); sr->addWidget(status, 1); meters_grid_->addLayout(sr, 1, 0);
    meters_grid_->setColumnStretch(0, 1);
    tx_meter_hold_ = new QTimer(this); tx_meter_hold_->setSingleShot(true); tx_meter_hold_->setInterval(150);
    connect(tx_meter_hold_, &QTimer::timeout, this, [this] { showTxMeter(false); });
}
void ReceiverPanel::showTxMeter(bool tx) { if (!tx_meter_) return; meter_->setVisible(!tx); meter_text_->setVisible(!tx); tx_meter_->setVisible(tx); }
void ReceiverPanel::setMeterTx(bool keyed) {      // called by the transmit card: keyed -> the power meter; back to the signal meter as the radio unkeys (150 ms, so a quick re-key does not flicker; the over's power is in the status line)
    if (!tx_meter_) return;
    if (keyed) { tx_meter_hold_->stop(); showTxMeter(true); }
    else if (tx_meter_->isVisibleTo(this) && !tx_meter_hold_->isActive()) tx_meter_hold_->start();
}
void ReceiverPanel::setRxLevelCal(bool valid, const std::array<double, 11>& meter, const std::array<double, 11>& preamp) {
    cal_valid_ = valid; cal_meter_ = meter; cal_preamp_ = preamp;
    meter_->setToolTip(valid ? "dBm from the radio's factory RX level table (EEPROM): per band, minus the preamp's gain when it is on. Not yet checked against a known signal."
                             : "dBm uses a placeholder offset: the radio's RX level table has not been read (it is read with the transmit check at start-up)");
}
void ReceiverPanel::setCheckedCal(int band, double meter, double preamp) {      // from the internal-generator check: used before the factory table on that band
    if (band < 0 || band >= 11) return; chk_valid_[size_t(band)] = true; chk_meter_[size_t(band)] = meter; chk_preamp_[size_t(band)] = preamp;
}
int ReceiverPanel::meterSource() const {           // 0 placeholder, 1 the EEPROM's factory table, 2 checked here with the internal generator
    const int b = fireside::txm::band_of(dialHz()); if (b < 0 || b >= 11) return 0;
    return chk_valid_[size_t(b)] ? 2 : cal_valid_ ? 1 : 0;
}
bool ReceiverPanel::meterCalibrated() const { return meterSource() > 0; }
double ReceiverPanel::meterOffsetDb() const {      // dBm = dBFS + this: the band's meter offset (+ the preamp row while the preamp is on), else the placeholder
    const int b = fireside::txm::band_of(dialHz()); const int src = meterSource();
    if (src == 0) return SMeter::kUncalOffsetDb;
    const bool pre = preamp_ && preamp_->isChecked(); const size_t i = size_t(b);
    if (src == 2) return chk_meter_[i] + (pre ? (std::isnan(chk_preamp_[i]) ? (cal_valid_ ? cal_preamp_[i] : 0.0) : chk_preamp_[i]) : 0.0);
    return cal_meter_[i] + kTableToDbfsDb + (pre ? cal_preamp_[i] : 0.0);
}
void ReceiverPanel::setTxLamp(QLabel* lamp) { if (mode_switch_col_) mode_switch_col_->insertWidget(0, lamp); }   // left of the frequency: the TX light
QWidget* ReceiverPanel::takeAdvancedWidget() { QWidget* w = advanced_; advanced_ = nullptr; return w; }
QWidget* ReceiverPanel::takeOutputWidget() { if (out_dest_) out_dest_->setParent(nullptr); return out_dest_; }
int ReceiverPanel::highlightedLines() const {
    int n = 0; for (QTextBlock b = dec_text_->document()->begin(); b.isValid(); b = b.next()) if (b.blockFormat().background().color() == QColor("#7a1f2b")) ++n; return n;
}
// tag the recent decoder lines from this station: green NEW, amber worked-before-but-not-here, grey already worked; above those, from the user's whole log, magenta
// new DXCC (pink on a new band), violet new continent, cyan new zone, blue new grid (fireside::award). The tag is appended once per line.
void ReceiverPanel::markWorked(const QString& call, int state) {
    if (state < 0 || !*fireside::award::tag_text(state)) return;
    { const QString bc = QString::fromStdString(fireside::qrz::base_call(call.toStdString())); if (tag_by_call_.size() > 500) tag_by_call_.clear(); tag_by_call_[bc] = state;
      for (auto& c : caller_list_) if (c.call == bc) c.tag = state;
      renderCallers(); }
    QTextBlock b = dec_text_->document()->lastBlock(); QTextCharFormat fmt; fmt.setForeground(tagColour(state)); if (state >= fireside::award::kNewDxcc) fmt.setFontWeight(QFont::Bold); int n = 0;
    for (; b.isValid() && n < 80; b = b.previous(), ++n) {
        const QString full = b.text(); const int sep = full.indexOf(kInfoSep); if (sep < 0) continue;             // the tag field follows the separator
        const int at = sep + int(kInfoSep.size()); if (!full.mid(at, kTagCols).trimmed().isEmpty()) continue;     // already tagged
        const auto m = fireside::logbook::parse_ftx_message(fireside::logbook::ftx_line_text(full.left(sep).toStdString())); if (!m.ok || QString::fromStdString(m.from) != call) continue;
        QTextCursor c(b); c.setPosition(b.position() + at); c.setPosition(std::min(b.position() + at + kTagCols, b.position() + int(full.size())), QTextCursor::KeepAnchor);
        c.insertText(QString(fireside::award::tag_text(state)).leftJustified(kTagCols), fmt);
    }
}
QString ReceiverPanel::heardGrid(const QString& call) const {
    const auto it = heard_grids_.constFind(QString::fromStdString(fireside::qrz::base_call(call.toStdString())));
    return it != heard_grids_.constEnd() && QDateTime::currentSecsSinceEpoch() - it->second < 2 * 3600 ? it->first : QString();
}
void ReceiverPanel::showCallInfo(const QString& call, const QString& text) {
    if (text.isEmpty()) return;
    { const QString b = QString::fromStdString(fireside::qrz::base_call(call.toStdString())); if (info_by_call_.size() > 500) info_by_call_.clear(); info_by_call_[b] = text;
      for (auto& c : caller_list_) if (c.call == b) c.info = text;                                    // the callers area shows the same callbook line
      renderCallers();
      if (b == qso_partner_ && qso_text_ && qso_start_block_ >= 0) {                                   // the QSO pane's "── QSO with CALL ──" line gets it too
          const QTextBlock hb = qso_text_->document()->findBlockByNumber(qso_start_block_); const QString head = QString::fromUtf8("\u2500\u2500 QSO with %1 \u2500\u2500").arg(b);
          if (hb.isValid() && hb.text() == head) { QTextCursor c(hb); c.movePosition(QTextCursor::EndOfBlock); QTextCharFormat f; f.setForeground(QColor("#8b93a7")); c.insertText("  " + text, f); }
      } }
    QTextCharFormat fmt; fmt.setForeground(QColor("#8b93a7")); fmt.setFontWeight(QFont::Normal); fmt.setBackground(Qt::transparent); int n = 0;
    if (decoderIndex() == 6) {                                                    // WSPR: after each spot of this station (its distance is on the line already)
        static const QRegularExpression km(",?\\s*\\d+ km( \\(QRZ\\))?$"); QString info = text; info.remove(km); if (info.trimmed().isEmpty()) return;
        for (QTextBlock b = dec_text_->document()->lastBlock(); b.isValid() && n < 200; b = b.previous(), ++n) {
            static const QRegularExpression dist("\\s+\\d+ km$");                       // our distance column ends the line: not part of the spot
            const QString line = b.text(); if (line.contains(kInfoSep)) continue;
            const auto sp = fireside::wspr::parse_wsprd_line(QString(line).remove(dist).toStdString());
            if (!sp || QString::fromStdString(fireside::qrz::base_call(sp->call)) != call) continue;
            QTextCursor c(b); c.movePosition(QTextCursor::EndOfBlock); c.insertText(kInfoSep + info, fmt);
        }
        return;
    }
    for (QTextBlock b = dec_text_->document()->lastBlock(); b.isValid() && n < 80; b = b.previous(), ++n) {
        const QString line = b.text(); const int sep = line.indexOf(kInfoSep);
        if (sep >= 0 && line.size() > sep + int(kInfoSep.size()) + kTagCols) continue;                           // info already there
        const auto m = fireside::logbook::parse_ftx_message(fireside::logbook::ftx_line_text((sep >= 0 ? line.left(sep) : line).toStdString())); if (!m.ok) continue;
        if (QString::fromStdString(fireside::qrz::base_call(m.from)) != call) continue;
        QTextCursor c(b); c.movePosition(QTextCursor::EndOfBlock);
        if (sep < 0) c.insertText(kInfoSep + QString(kTagCols, ' '), fmt);                                        // an old-format line: give it the columns first
        else if (line.size() < sep + int(kInfoSep.size()) + kTagCols) c.insertText(QString(sep + int(kInfoSep.size()) + kTagCols - int(line.size()), ' '), fmt);
        c.insertText(text, fmt);
    }
}
bool ReceiverPanel::eventFilter(QObject* o, QEvent* e) {
    if (e->type() == QEvent::MouseButtonDblClick && isFtIndex(decoderIndex()) && o->property("queue_line").isValid()) {   // a queue row: reply now
        activateLine(o->property("queue_line").toString(), true); return true;
    }
    if ((o == dec_text_->viewport() || (qso_text_ && o == qso_text_->viewport())) && e->type() == QEvent::MouseButtonDblClick && isFtIndex(decoderIndex())) {   // FT8 / FT4: double-click a decoded line -> reply (+ the Log tab), or queue it during a QSO
        auto* view = o == dec_text_->viewport() ? dec_text_ : qso_text_;
        const auto* me = static_cast<QMouseEvent*>(e); QString line = view->cursorForPosition(me->position().toPoint()).block().text();
        if (view == qso_text_ && line.contains("  TX ")) return true;                                               // our own transmission: nothing to answer
        activateLine(line, false);
        return true;
    }
    return QWidget::eventFilter(o, e);
}

void ReceiverPanel::setTransmitWidget(QWidget* w) {
    tx_w_ = w; tx_w_->setVisible(false); tx_btn_->setVisible(true);
    auto* root = qobject_cast<QVBoxLayout*>(layout()); root->insertWidget(root->indexOf(dec_row_ ? static_cast<QWidget*>(dec_row_) : static_cast<QWidget*>(dec_card_)) + 1, w);       // straight under the decoder, above the control deck
    connect(tx_btn_, &QPushButton::toggled, this, [this](bool on) { setTransmitOpen(on); });
}
void ReceiverPanel::setTransmitOpen(bool on) { if (!tx_w_) return; { QSignalBlocker b(tx_btn_); tx_btn_->setChecked(on); } tx_w_->setVisible(on); tx_btn_->setText(on ? QStringLiteral("TX lab  \u25BE") : QStringLiteral("TX lab  \u25B8")); }
void ReceiverPanel::setTransmitParts(QWidget* digi, QWidget* voice) {
    tx_side_ = digi;
    auto* root = qobject_cast<QVBoxLayout*>(layout());
    { auto* l = new QVBoxLayout; l->addWidget(voice); voice_card_ = theme::makeCard("Voice transmit", l); root->insertWidget(root->indexOf(dec_card_), voice_card_); }   // under the waterfall, voice only
    const int at = root->indexOf(dec_card_); root->removeWidget(dec_card_);
    dec_row_ = new QWidget; auto* h = new QHBoxLayout(dec_row_); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(12);
    auto* l = new QVBoxLayout; l->addWidget(digi); send_card_ = theme::makeCard("Send FT8 / FT4", l); send_card_->setFixedWidth(520);
    h->addWidget(dec_card_, 1); h->addWidget(send_card_, 0); root->insertWidget(at, dec_row_);
    dec_card_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setDecoderIndex(decoderIndex());                               // re-apply the decoder list's sizing
    syncGroup();
}
void ReceiverPanel::setTxMute(bool on) { tx_mute_ = on; }
void ReceiverPanel::setCwPart(QWidget* w) {
    if (!dec_row_ || cw_card_) return;
    auto* l = new QVBoxLayout; l->addWidget(w); cw_card_ = theme::makeCard("Send CW", l); cw_card_->setFixedWidth(520);
    if (auto* h = qobject_cast<QHBoxLayout*>(dec_row_->layout())) h->addWidget(cw_card_, 0);
    syncGroup();
}
int ReceiverPanel::modeGroup() const { return std::max(0, grp_mode_->checkedId()); }
void ReceiverPanel::applyGroup(int g) {                              // what each group shows; nothing here changes the radio
    if (!grp_mode_) return; { QSignalBlocker b(grp_mode_); if (auto* bt = grp_mode_->button(g)) bt->setChecked(true); }
    mode_pages_->setCurrentIndex(g);
    if (auto* t = mode_card_->findChild<QLabel*>("cardTitle")) t->setText(g == 0 ? "VOICE MODES" : g == 1 ? "CW" : "DIGITAL MODES");
    if (dec_row_) dec_row_->setVisible(g != 0);                       // the decoder: CW (Morse) and Digital only
    if (send_card_) send_card_->setVisible(g == 2);                   // the FT8 / FT4 message controls: Digital only
    if (cw_card_) cw_card_->setVisible(g == 1);                       // the CW transmit controls: CW only
    if (voice_card_) voice_card_->setVisible(g == 0);                 // the voice transmit controls: Voice only
}
void ReceiverPanel::syncGroup() {                                    // the group the receiver's own state belongs to (a band recall, a memory, a decoder pick)
    const int d = decoderIndex(); int g = modeGroup();
    if (d >= 2) g = 2; else if (d == 1 || modeIndex() == 3) g = 1; else if (!(external_ && g == 2)) g = 0;   // decoder off in External mode: stay in Digital
    if (g == 0 && modeIndex() != 3) last_voice_mode_ = modeIndex() == 1 ? 1 : (modeIndex() == 0 ? 0 : last_voice_mode_);
    if (g == 2 && d >= 2) last_digi_ = d;
    applyGroup(g);
}
void ReceiverPanel::chooseGroup(int g) {                             // the user clicked Voice / CW / Digi: go to a sensible mode of that group
    if (g == 0) { if (decoderIndex() != 0) setDecoderIndex(0); setModeIndex(modeIndex() == 3 ? last_voice_mode_ : modeIndex()); }
    else if (g == 1) { if (decoderIndex() > 1) setDecoderIndex(0); setModeIndex(3); }
    else { if (last_digi_ == 4 || last_digi_ == 5) selectFt(last_digi_ == 5); else if (last_digi_ == 6) selectWspr(); else setDecoderIndex(last_digi_); }
    syncGroup(); applyGroup(g);
}
bool ReceiverPanel::transmitOpen() const { return tx_w_ && tx_w_->isVisibleTo(this); }
void ReceiverPanel::setLookupWidget(QWidget* w) { log_slot_->addWidget(w, 1); log_card_->setVisible(true); }
void ReceiverPanel::setLogFormWidget(QWidget* w) { form_slot_->addWidget(w); }
void ReceiverPanel::setLogActive(bool on) {                                  // while the log form is open it takes the Band and Mode cards' place
    form_card_->setVisible(on);
}
bool ReceiverPanel::logActive() const { return form_card_->isVisibleTo(const_cast<ReceiverPanel*>(this)); }
