/* This file is part of the KDE project
   Copyright (C) 2008 Thorsten Zachmann <zachmann@kde.org>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
*/

#ifndef KOSHAREDSAVINGDATA_H
#define KOSHAREDSAVINGDATA_H

#include "kritaflake_export.h"

/**
 * The KoSharedSavingData class is used to share data between shapes during saving.
 * These data can be added to the KoShapeSavingContext using KoShapeSavingContext::addSharedData().
 * A different shape can then get the data from the context using KoShapeSavingContext::sharedData().
 */
class KRITAFLAKE_EXPORT KoSharedSavingData
{
public:
    KoSharedSavingData();
    virtual ~KoSharedSavingData();
};

#endif /* KOSHAREDSAVINGDATA_H */
