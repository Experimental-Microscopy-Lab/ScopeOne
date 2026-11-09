#pragma once

#include <QWidget>

class QListWidget;
namespace scopeone::core { class ScopeOneCore; }

namespace scopeone::ui
{
    class LightSourcesPage : public QWidget
    {
    public:
        explicit LightSourcesPage(scopeone::core::ScopeOneCore* core, QWidget* parent = nullptr);
    private:
        void refresh();
        void edit(bool existing);
        scopeone::core::ScopeOneCore* m_core;
        QListWidget* m_list;
    };
}
