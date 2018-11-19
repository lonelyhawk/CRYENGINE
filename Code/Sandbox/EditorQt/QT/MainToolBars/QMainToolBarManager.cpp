// Copyright 2001-2018 Crytek GmbH / Crytek Group. All rights reserved.

#include "StdAfx.h"
#include "QMainToolBarManager.h"

#include "IEditorImpl.h"
#include "LevelEditor/LevelFileUtils.h"
#include "QT/QtMainFrame.h"

#include <CryIcon.h>

#include <Commands/CustomCommand.h>
#include "Commands/CommandManager.h"
#include <Commands/QCommandAction.h>
#include <FileUtils.h>
#include <PathUtils.h>
#include <QtUtil.h>
#include <Util/UserDataUtil.h>

#include <CryString/CryPath.h>
#include <CrySystem/IProjectManager.h>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QToolBar>
#include <QToolButton>

namespace Private_ToolbarManager
{
static const char* szUserToolbarsPath = "Toolbars";
static const char* s_defaultPath = "Editor/ToolBars";
static const char* s_actionPropertyName = "cvarDescValue";
static const char* s_actionCVarBitFlagName = "cvarDescIsBitFlag";
static const int s_version = 2;
}

#define ADD_TO_VARIANT_MAP(var, map) map[ # var] = var

QMainToolBarManager::QCommandDesc::QCommandDesc(const QVariantMap& variantMap, int version)
	: command(variantMap["command"].toString())
	, m_IsDeprecated(false)
{
	if (version == 1)
	{
		name = variantMap["uiName"].toString();
		iconPath = variantMap["icon"].toString();
		m_IsCustom = variantMap["custom"].toBool();
	}
	else
	{
		name = variantMap["name"].toString();
		iconPath = variantMap["iconPath"].toString();
		m_IsCustom = variantMap["bIsCustom"].toBool();
	}

	// Check to see if the command needs to be remapped
	m_IsDeprecated = IsDeprecated();
	if (m_IsDeprecated)
	{
		CCommand* pCommand = GetIEditorImpl()->GetCommandManager()->GetCommand(command.toLocal8Bit());
		// Discard serialized data and get all relevant data directly from new command
		if (pCommand)
		{
			InitFromCommand(pCommand);
		}
	}
}

QMainToolBarManager::QCommandDesc::QCommandDesc(const CCommand* pCommand)
{
	InitFromCommand(pCommand);
}

void QMainToolBarManager::QCommandDesc::InitFromCommand(const CCommand* pCommand)
{
	const CUiCommand* pUiCommand = static_cast<const CUiCommand*>(pCommand);
	CUiCommand::UiInfo* pInfo = pUiCommand->GetUiInfo();

	name = QtUtil::ToQString(pCommand->GetName());
	command = QtUtil::ToQString(pCommand->GetCommandString());
	iconPath = QtUtil::ToQString(pInfo->icon);
	m_IsCustom = pCommand->IsCustomCommand();
}

QVariant QMainToolBarManager::QCommandDesc::ToVariant() const
{
	QVariantMap map;
	ADD_TO_VARIANT_MAP(name, map);
	ADD_TO_VARIANT_MAP(command, map);
	ADD_TO_VARIANT_MAP(iconPath, map);
	ADD_TO_VARIANT_MAP(m_IsCustom, map);
	map["type"] = Command;
	return map;
}

QCommandAction* QMainToolBarManager::QCommandDesc::ToQCommandAction() const
{
	CCommand* pCommand = GetIEditorImpl()->GetCommandManager()->GetCommand(command.toLocal8Bit());
	if (!pCommand && IsCustom())
	{
		string commandNameStr = QtUtil::ToString(name);
		string commandStr = QtUtil::ToString(command);
		CCustomCommand* pCustomCommand = new CCustomCommand(commandNameStr, commandStr);
		pCustomCommand->Register();
		pCommand = pCustomCommand;
	}

	if (!pCommand || !pCommand->CanBeUICommand())
		return nullptr;

	CUiCommand* pUiCommand = static_cast<CUiCommand*>(pCommand);
	CUiCommand::UiInfo* pInfo = pUiCommand->GetUiInfo();

	if (!pInfo)
		return nullptr;

	QCommandAction* pAction = static_cast<QCommandAction*>(pInfo);

	return pAction;
}

bool QMainToolBarManager::QCommandDesc::IsDeprecated() const
{
	return m_IsDeprecated || GetIEditorImpl()->GetCommandManager()->IsCommandDeprecated(command.toLocal8Bit());
}

void QMainToolBarManager::QCommandDesc::SetName(const QString& n)
{
	name = n;
	commandChangedSignal();
}

void QMainToolBarManager::QCommandDesc::SetIcon(const QString& path)
{
	iconPath = path;
	commandChangedSignal();
}

//////////////////////////////////////////////////////////////////////////

QMainToolBarManager::QCVarDesc::QCVarDesc(const QVariantMap& variantMap, int version)
	: name(variantMap["name"].toString())
	, iconPath(variantMap["iconPath"].toString())
	, value(variantMap["value"])
	, isBitFlag(false)
{
	if (variantMap.contains("isBitFlag"))
		isBitFlag = variantMap["isBitFlag"].toBool();
}

QVariant QMainToolBarManager::QCVarDesc::ToVariant() const
{
	QVariantMap map;
	ADD_TO_VARIANT_MAP(name, map);
	ADD_TO_VARIANT_MAP(iconPath, map);
	ADD_TO_VARIANT_MAP(value, map);
	ADD_TO_VARIANT_MAP(isBitFlag, map);
	map["type"] = CVar;
	return map;
}

void QMainToolBarManager::QCVarDesc::SetCVar(const QString& cvar)
{
	name = cvar;
	cvarChangedSignal();
}

void QMainToolBarManager::QCVarDesc::SetCVarValue(const QVariant& cvarValue)
{
	value = cvarValue;
	cvarChangedSignal();
}

void QMainToolBarManager::QCVarDesc::SetIcon(const QString& path)
{
	iconPath = path;
	cvarChangedSignal();
}

//////////////////////////////////////////////////////////////////////////

QMainToolBarManager::QToolBarDesc::QToolBarDesc(const QVariantList& commandList, int version)
	: updated(false)
{
	for (const QVariant& commandVar : commandList)
	{
		std::shared_ptr<QItemDesc> pItem = CreateItem(commandVar, version);
		if (!pItem)
			continue;

		items.push_back(pItem);
	}
}

std::shared_ptr<QMainToolBarManager::QItemDesc> QMainToolBarManager::QToolBarDesc::CreateItem(const QVariant& itemVariant, int version)
{
	if (itemVariant.type() == QVariant::String)
	{
		if (itemVariant.toString() == "separator")
		{
			return std::make_shared<QSeparatorDesc>();
		}
	}

	QVariantMap itemMap = itemVariant.toMap();
	if (itemMap["type"].toInt() == QItemDesc::Command)
	{
		std::shared_ptr<QCommandDesc> pCommand = std::make_shared<QCommandDesc>(itemMap, version);
		pCommand->commandChangedSignal.Connect(this, &QToolBarDesc::OnCommandChanged);
		return pCommand;
	}
	else if (itemMap["type"].toInt() == QItemDesc::CVar)
	{
		std::shared_ptr<QCVarDesc> pCVar = std::make_shared<QCVarDesc>(itemMap, version);
		pCVar->cvarChangedSignal.Connect(this, &QToolBarDesc::OnCommandChanged);
		return pCVar;
	}

	return nullptr;
}

QVariant QMainToolBarManager::QToolBarDesc::ToVariant() const
{
	QVariantList commandsVarList;
	for (std::shared_ptr<QItemDesc> pItem : items)
	{
		commandsVarList.push_back(pItem->ToVariant());
	}

	return commandsVarList;
}

int QMainToolBarManager::QToolBarDesc::IndexOfItem(std::shared_ptr<QItemDesc> pItem)
{
	return items.indexOf(pItem);
}

void QMainToolBarManager::QToolBarDesc::MoveItem(int currIdx, int idx)
{
	if (idx == -1) // Move to the end
		idx = items.size() - 1;

	std::shared_ptr<QItemDesc> pItem = items.takeAt(currIdx);

	if (currIdx < idx)
		--idx;

	items.insert(idx, pItem);
}

int QMainToolBarManager::QToolBarDesc::IndexOfCommand(const CCommand* pCommand)
{
	for (auto i = 0; i < items.size(); ++i)
	{
		const std::shared_ptr<QItemDesc> pItem = items[i];
		if (pItem->GetType() != QMainToolBarManager::QItemDesc::Command)
			continue;

		const std::shared_ptr<QCommandDesc> pCommandDesc = std::static_pointer_cast<QCommandDesc>(pItem);
		if (pCommandDesc->GetCommand() == QtUtil::ToQString(pCommand->GetCommandString()))
			return i;
	}

	return -1;
}

void QMainToolBarManager::QToolBarDesc::InsertItem(const QVariant& itemVariant, int idx)
{
	InsertItem(CreateItem(itemVariant, Private_ToolbarManager::s_version), idx);
}

void QMainToolBarManager::QToolBarDesc::InsertItem(std::shared_ptr<QItemDesc> pItem, int idx)
{
	if (idx < 0)
	{
		items.push_back(pItem);
		return;
	}

	items.insert(idx, pItem);
}

void QMainToolBarManager::QToolBarDesc::InsertCommand(const CCommand* pCommand, int idx)
{
	int currIdx = IndexOfCommand(pCommand);

	// If the item is already in the toolbar than ignore
	if (currIdx != -1)
		return;

	std::shared_ptr<QCommandDesc> pCommandDesc = std::make_shared<QCommandDesc>(pCommand);
	InsertItem(pCommandDesc, idx);
	pCommandDesc->commandChangedSignal.Connect(this, &QToolBarDesc::OnCommandChanged);
}

void QMainToolBarManager::QToolBarDesc::InsertCVar(const QString& cvarName, int idx)
{
	std::shared_ptr<QCVarDesc> pCVar = std::make_shared<QCVarDesc>();
	pCVar->cvarChangedSignal.Connect(this, &QToolBarDesc::OnCommandChanged);
	InsertItem(pCVar, idx);
	return;
}

void QMainToolBarManager::QToolBarDesc::InsertSeparator(int idx)
{
	InsertItem(std::make_shared<QSeparatorDesc>(), idx);
}

void QMainToolBarManager::QToolBarDesc::RemoveItem(std::shared_ptr<QItemDesc> pItem)
{
	int idx = items.indexOf(pItem);
	if (idx < 0)
		return;

	items.remove(idx);
}

void QMainToolBarManager::QToolBarDesc::RemoveItem(int idx)
{
	items.remove(idx);
}

bool QMainToolBarManager::QToolBarDesc::RequiresUpdate() const
{
	if (updated)
	{
		return false;
	}

	for (auto i = 0; i < items.size(); ++i)
	{
		const std::shared_ptr<QItemDesc> pItem = items[i];
		if (pItem->GetType() != QMainToolBarManager::QItemDesc::Command)
			continue;

		const std::shared_ptr<QCommandDesc> pCommandDesc = std::static_pointer_cast<QCommandDesc>(pItem);
		if (pCommandDesc->IsDeprecated())
		{
			return true;
		}
	}

	return false;
}

//////////////////////////////////////////////////////////////////////////

QMainToolBarManager::QMainToolBarManager(CEditorMainFrame* pMainFrame)
	: QObject(pMainFrame)
	, CUserData({ Private_ToolbarManager::szUserToolbarsPath })
	, m_pMainFrame(pMainFrame)
{
	LoadAll();
}

QMainToolBarManager::~QMainToolBarManager()
{
	for (const QMetaObject::Connection& connection : m_cvarActionConnections)
		disconnect(connection);
}

void QMainToolBarManager::AddToolBar(const QString& name, const std::shared_ptr<QToolBarDesc>& pToolBar)
{
	m_ToolBarsDesc[name] = pToolBar;
	SaveToolBar(name);
	CreateMainFrameToolBar(name, pToolBar);
}

void QMainToolBarManager::UpdateToolBar(const std::shared_ptr<QToolBarDesc>& pToolBar)
{
	SaveToolBar(pToolBar->GetName());
	CreateMainFrameToolBar(pToolBar->GetName(), pToolBar);
}

void QMainToolBarManager::RemoveToolBar(const QString& name)
{
	m_ToolBarsDesc.remove(name);

	QString fullPath(UserDataUtil::GetUserPath(PathUtil::Make(Private_ToolbarManager::szUserToolbarsPath, QtUtil::ToConstCharPtr(name), ".json").c_str()));
	FileUtils::Remove(QtUtil::ToConstCharPtr(fullPath));

	CEditorMainFrame* pMainFrame = CEditorMainFrame::GetInstance();
	QToolBar* pToolBar = pMainFrame->findChild<QToolBar*>(name + "ToolBar");
	if (pToolBar)
	{
		pMainFrame->removeToolBar(pToolBar);
		pToolBar->deleteLater();
	}
}

void QMainToolBarManager::SaveToolBar(const QString& name) const
{
	const std::shared_ptr<QMainToolBarManager::QToolBarDesc> toolBar = m_ToolBarsDesc[name];
	QVariantMap toDisk;
	toDisk["version"] = Private_ToolbarManager::s_version;
	toDisk["toolBar"] = toolBar->ToVariant();

	QString path(PathUtil::Make(Private_ToolbarManager::szUserToolbarsPath, QtUtil::ToConstCharPtr(name), ".json").c_str());

	QJsonDocument doc(QJsonDocument::fromVariant(toDisk));
	UserDataUtil::Save(path.toLocal8Bit(), doc.toJson());
}

void QMainToolBarManager::LoadAll()
{
	// Load built in toolbars
	LoadToolBarsFromDir(PathUtil::Make(PathUtil::GetEnginePath(), Private_ToolbarManager::s_defaultPath).c_str());

	// Load game project specific toolbars
	QString projectPath(GetIEditorImpl()->GetProjectManager()->GetCurrentProjectDirectoryAbsolute());
	projectPath = projectPath + "/" + Private_ToolbarManager::s_defaultPath;
	LoadToolBarsFromDir(projectPath);

	// Load user toolbars
	LoadToolBarsFromDir(UserDataUtil::GetUserPath("ToolBars"));
}

void QMainToolBarManager::LoadToolBarsFromDir(const QString& dirPath)
{
	QDir dir(dirPath);
	if (!dir.exists())
		return;

	QFileInfoList infoList = dir.entryInfoList(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);

	for (const QFileInfo& fileInfo : infoList)
	{
		QString path = fileInfo.absoluteFilePath();
		if (fileInfo.isDir())
		{
			LoadToolBarsFromDir(path);
			continue;
		}

		QFile file(path);
		if (!file.open(QIODevice::ReadOnly))
		{
			QString msg = "Failed to open path: " + path;
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, msg.toLocal8Bit());
			continue;
		}

		QJsonDocument doc(QJsonDocument::fromJson(file.readAll()));
		QVariantMap variantMap = doc.toVariant().toMap();
		int loadedVersion = variantMap["version"].toInt();

		if (loadedVersion == 1)
		{
			QVariantMap toolBar = variantMap["toolBar"].toMap();
			std::shared_ptr<QToolBarDesc> toolBarDesc = std::make_shared<QToolBarDesc>(toolBar["Commands"].toList(), loadedVersion);
			toolBarDesc->toolBarChangedSignal.Connect([this, toolBarDesc](const QToolBarDesc*)
			{
				UpdateToolBar(toolBarDesc);
			});
			toolBarDesc->SetName(fileInfo.baseName());
			m_ToolBarsDesc[fileInfo.baseName()] = toolBarDesc;
			continue;
		}

		QVariantList toolBar = variantMap["toolBar"].toList();
		std::shared_ptr<QToolBarDesc> toolBarDesc = std::make_shared<QToolBarDesc>(toolBar, loadedVersion);
		toolBarDesc->toolBarChangedSignal.Connect([this, toolBarDesc](const QToolBarDesc*)
		{
			UpdateToolBar(toolBarDesc);
		});
		toolBarDesc->SetName(fileInfo.baseName());
		m_ToolBarsDesc[fileInfo.baseName()] = toolBarDesc;
	}
}

void QMainToolBarManager::CreateMainFrameToolBars()
{
	bool requiresUpdate = false;
	for (QMap<QString, std::shared_ptr<QToolBarDesc>>::const_iterator ite = m_ToolBarsDesc.begin(); ite != m_ToolBarsDesc.end(); ++ite)
	{
		const std::shared_ptr<QToolBarDesc> toolBarDesc = ite.value();

		CreateMainFrameToolBar(ite.key(), toolBarDesc);

		requiresUpdate |= toolBarDesc->RequiresUpdate();
	}

	if (!requiresUpdate)
	{
		return;
	}

	for (QMap<QString, std::shared_ptr<QToolBarDesc>>::const_iterator ite = m_ToolBarsDesc.begin(); ite != m_ToolBarsDesc.end(); ++ite)
	{
		const std::shared_ptr<QToolBarDesc> toolBarDesc = ite.value();
		if (!toolBarDesc->RequiresUpdate())
		{
			continue;
		}

		UpdateToolBarOnDisk(ite.key(), toolBarDesc);
		toolBarDesc->MarkAsUpdated();
	}
}

void QMainToolBarManager::UpdateToolBarOnDisk(const QString& name, const std::shared_ptr<QToolBarDesc>& pToolBarDesc) const
{
	QVariantMap toDisk;
	toDisk["version"] = Private_ToolbarManager::s_version;
	toDisk["toolBar"] = pToolBarDesc->ToVariant();

	QJsonDocument doc(QJsonDocument::fromVariant(toDisk));
	UserDataUtil::Save(PathUtil::Make(Private_ToolbarManager::szUserToolbarsPath, QtUtil::ToConstCharPtr(name), ".json").c_str(), doc.toJson());
}

void QMainToolBarManager::CreateMainFrameToolBar(const QString& name, const std::shared_ptr<QToolBarDesc>& pToolBarDesc)
{
	// Check if the toolbar already exists on the mainframe
	QString toolBarObjectName = name + "ToolBar";
	QToolBar* pToolBar = m_pMainFrame->findChild<QToolBar*>(toolBarObjectName);
	bool bToolBarExists = pToolBar != nullptr;

	// Create the toolbar if the mainframe doesn't have one already
	if (!pToolBar)
	{
		pToolBar = new QToolBar();
		pToolBar->setWindowTitle(name);
		pToolBar->setObjectName(toolBarObjectName);
	}
	else
	{
		pToolBar->clear();
	}

	CreateToolBar(pToolBarDesc, pToolBar);

	if (bToolBarExists)
		return;

	m_pMainFrame->addToolBar(pToolBar);
	pToolBar->setVisible(false);
}

void QMainToolBarManager::CreateToolBar(const std::shared_ptr<QToolBarDesc>& pToolBarDesc, QToolBar* pToolBar)
{
	for (std::shared_ptr<QItemDesc> pItemDesc : pToolBarDesc->GetItems())
	{
		switch (pItemDesc->GetType())
		{
		case QItemDesc::Command:
			{
				std::shared_ptr<QCommandDesc> pCommandDesc = std::static_pointer_cast<QCommandDesc>(pItemDesc);
				QCommandAction* pAction = pCommandDesc->ToQCommandAction();
				if (!pAction)
				{
					CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "Couldn't create tool bar button for command: %s", pCommandDesc->GetCommand().toStdString().c_str());
					continue;
				}

				const QString& iconPath = pCommandDesc->GetIcon();
				if (!iconPath.isEmpty())
				{
					pAction->UiInfo::icon = QtUtil::ToString(iconPath);
					pAction->setIcon(CryIcon(iconPath));
				}
				pToolBar->addAction(pAction);
				break;
			}
		case QItemDesc::CVar:
			{
				std::shared_ptr<QCVarDesc> pCVarDesc = std::static_pointer_cast<QCVarDesc>(pItemDesc);
				const QString& iconPath = pCVarDesc->GetIcon();
				CryIcon icon(iconPath.isEmpty() ? "icons:General/File.ico" : iconPath);
				QAction* pAction = new QAction(icon, QString("%1 %2").arg(pCVarDesc->GetName(), pCVarDesc->GetValue().toString()), pToolBar);
				pAction->setCheckable(true);
				ICVar* pCVar = gEnv->pConsole->GetCVar(pCVarDesc->GetName().toStdString().c_str());

				if (pCVar)
				{
					std::vector<QAction*> actions;
					auto ite = m_pCVarActions.find(pCVar);
					if (ite != m_pCVarActions.end())
						actions = ite.value();
					else
						pCVar->AddOnChange([this, pCVar]
						{
							this->OnCVarChanged(pCVar);
						});

					pAction->setProperty(Private_ToolbarManager::s_actionPropertyName, pCVarDesc->GetValue());
					pAction->setProperty(Private_ToolbarManager::s_actionCVarBitFlagName, pCVarDesc->IsBitFlag());
					actions.push_back(pAction);
					m_pCVarActions.insert(pCVar, actions);

					m_cvarActionConnections.push_back(connect(pAction, &QAction::destroyed, [this, pCVar](QObject* pObject)
					{
						this->OnCVarActionDestroyed(pCVar, static_cast<QAction*>(pObject));
					}));

					OnCVarChanged(pCVar);
				}

				connect(pAction, &QAction::triggered, [this, pAction, pCVarDesc, pCVar](bool bChecked)
				{
					if (!pCVar)
						return;

					QVariant variant = pCVarDesc->GetValue();
					switch (pCVar->GetType())
					{
					case ECVarType::Int:
						{
						  int currValue = pCVar->GetIVal();
						  int newValue = variant.toInt();

						  bool isBitFlag = pCVarDesc->IsBitFlag();

						  if (isBitFlag)
								pCVar->Set(currValue & newValue ? currValue & ~newValue : currValue | newValue);
						  else
								pCVar->Set(pCVar->GetIVal() == newValue ? 0 : newValue);
						}
						break;
					case ECVarType::Float:
						pCVar->Set(pCVar->GetFVal() == variant.toFloat() ? 0.0f : variant.toFloat());
						break;
					case ECVarType::String:
						pCVar->Set(variant.toString().toStdString().c_str());
					case ECVarType::Int64:
						CRY_ASSERT_MESSAGE(false, "QMainToolBarManager::CreateToolBar int64 cvar not implemented");
						break;
					default:
						break;
					}

					OnCVarChanged(pCVar);
				});
				pToolBar->addAction(pAction);
				break;
			}
		case QItemDesc::Separator:
			pToolBar->addSeparator();
			break;
		default:
			break;
		}
	}
}

void QMainToolBarManager::OnCVarActionDestroyed(ICVar* pCVar, QAction* pObject)
{
	auto ite = m_pCVarActions.find(pCVar);
	if (ite == m_pCVarActions.end())
		return;

	std::vector<QAction*>& actions = ite.value();
	auto actionIte = std::remove(actions.begin(), actions.end(), pObject);
	if (actionIte != actions.end())
		actions.erase(actionIte);
}

void QMainToolBarManager::OnCVarChanged(ICVar* pCVar)
{
	auto ite = m_pCVarActions.find(pCVar);
	if (ite == m_pCVarActions.end())
		return;

	std::vector<QAction*>& actions = ite.value();

	bool bChecked = false;
	switch (pCVar->GetType())
	{
	case ECVarType::Int:
		for (QAction* pAction : actions)
		{
			bool isBitFlag = pAction->property(Private_ToolbarManager::s_actionCVarBitFlagName).toBool();
			int value = pAction->property(Private_ToolbarManager::s_actionPropertyName).toInt();
			pAction->setChecked(isBitFlag ? value & pCVar->GetIVal() : value == pCVar->GetIVal());
		}
		break;
	case ECVarType::Float:
		for (QAction* pAction : actions)
			pAction->setChecked(pAction->property(Private_ToolbarManager::s_actionPropertyName) == pCVar->GetFVal());
		break;
	case ECVarType::String:
		for (QAction* pAction : actions)
			pAction->setChecked(pAction->property(Private_ToolbarManager::s_actionPropertyName) == QString(pCVar->GetString()));
		break;
	case ECVarType::Int64:
		for (QAction* pAction : actions)
		{
			bool isBitFlag = pAction->property(Private_ToolbarManager::s_actionCVarBitFlagName).toBool();
			int64 value = pAction->property(Private_ToolbarManager::s_actionPropertyName).toLongLong();
			pAction->setChecked(isBitFlag ? value & pCVar->GetI64Val() : value == pCVar->GetI64Val());
		}
		break;
	default:
		break;
	}
}
