#ifndef PUSHBUTTON_H
#define PUSHBUTTON_H

#include <QWidget>

class QPushButton;

// This is the declaration of our MainWidget class
// The definition/implementation is in mainwidget.cpp
class QPushButton : public QWidget
{
    Q_OBJECT

public:
    explicit QPushButton(QWidget *parent = 0); //Constructor
    ~PushButton(); // Destructor

private:
   QPushButton* button_;
};

#endif // PUSHBUTTON_H
